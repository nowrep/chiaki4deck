// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <streamsession.h>
#include <settings.h>
#include <controllermanager.h>

#include <chiaki/base64.h>
#include <chiaki/streamconnection.h>

#include <QKeyEvent>

#include <cstring>
#include <chiaki/session.h>
#include <chiaki/time.h>

#define SETSU_UPDATE_INTERVAL_MS 4
#define STEAMDECK_UPDATE_INTERVAL_MS 4
#define STEAMDECK_HAPTIC_INTERVAL_MS 10 // check every interval
#define STEAMDECK_HAPTIC_PACKETS_PER_ANALYSIS 4 // send packets every interval * packets per analysis
#define STEAMDECK_HAPTIC_SAMPLING_RATE 3000
// DualShock4 touchpad is 1920 x 942
#define PS4_TOUCHPAD_MAX_X 1920.0f
#define PS4_TOUCHPAD_MAX_Y 942.0f
// DualSense touchpad is 1919 x 1079
#define PS5_TOUCHPAD_MAX_X 1919.0f
#define PS5_TOUCHPAD_MAX_Y 1079.0f

#define MICROPHONE_SAMPLES 480
#ifdef Q_OS_LINUX
#define DUALSENSE_AUDIO_DEVICE_NEEDLE "DualSense"
#else
#define DUALSENSE_AUDIO_DEVICE_NEEDLE "Wireless Controller"
#endif
#if CHIAKI_GUI_ENABLE_SPEEX
#define ECHO_QUEUE_MAX 40
#endif

StreamSessionConnectInfo::StreamSessionConnectInfo(
		Settings *settings,
		ChiakiTarget target,
		QString host,
		QByteArray regist_key,
		QByteArray morning,
		QString initial_login_pin,
		bool fullscreen, 
		bool zoom, 
		bool stretch)
	: settings(settings)
{
	key_map = settings->GetControllerMappingForDecoding();
	decoder = settings->GetDecoder();
	hw_decoder = settings->GetHardwareDecoder();
	hw_device_ctx = nullptr;
	audio_out_device = settings->GetAudioOutDevice();
	audio_in_device = settings->GetAudioInDevice();
	log_level_mask = settings->GetLogLevelMask();
	log_file = CreateLogFilename();
	video_profile = settings->GetVideoProfile();
	this->target = target;
	this->host = host;
	this->regist_key = regist_key;
	this->morning = morning;
	this->initial_login_pin = initial_login_pin;
	audio_buffer_size = settings->GetAudioBufferSize();
	this->fullscreen = fullscreen;
	this->zoom = zoom;
	this->stretch = stretch;
	this->enable_keyboard = false; // TODO: from settings
	this->enable_dualsense = settings->GetDualSenseEnabled();
	this->buttons_by_pos = settings->GetButtonsByPosition();
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	this->vertical_sdeck = settings->GetVerticalDeckEnabled();
#endif
#if CHIAKI_GUI_ENABLE_SPEEX
	this->speech_processing_enabled = settings->GetSpeechProcessingEnabled();
	this->noise_suppress_level = settings->GetNoiseSuppressLevel();
	this->echo_suppress_level = settings->GetEchoSuppressLevel();
#endif
}

static void AudioSettingsCb(uint32_t channels, uint32_t rate, void *user);
static void AudioFrameCb(int16_t *buf, size_t samples_count, void *user);
static void HapticsFrameCb(uint8_t *buf, size_t buf_size, void *user);
static void EventCb(ChiakiEvent *event, void *user);
#if CHIAKI_GUI_ENABLE_SETSU
static void SessionSetsuCb(SetsuEvent *event, void *user);
#endif
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
static void SessionSDeckCb(SDeckEvent *event, void *user);
#endif
static void FfmpegFrameCb(ChiakiFfmpegDecoder *decoder, void *user);

StreamSession::StreamSession(const StreamSessionConnectInfo &connect_info, QObject *parent)
	: QObject(parent),
	log(this, connect_info.log_level_mask, connect_info.log_file),
	ffmpeg_decoder(nullptr),
#if CHIAKI_LIB_ENABLE_PI_DECODER
	pi_decoder(nullptr),
#endif
	audio_out(0),
	audio_in(0),
	haptics_output(0),
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	sdeck_haptics_senderl(nullptr),
	sdeck_haptics_senderr(nullptr),
	haptics_sdeck(0),
#endif
#if CHIAKI_GUI_ENABLE_SPEEX
	echo_resampler_buf(nullptr),
	mic_resampler_buf(nullptr),
#endif
	haptics_resampler_buf(nullptr)
{
	mic_buf.buf = nullptr;
	connected = false;
	muted = true;
	mic_connected = false;
	allow_unmute = false;
	input_blocked = false;
	ChiakiErrorCode err;
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
    haptics_sdeck = 0;
#endif
#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(connect_info.decoder == Decoder::Pi)
	{
		pi_decoder = CHIAKI_NEW(ChiakiPiDecoder);
		if(chiaki_pi_decoder_init(pi_decoder, log.GetChiakiLog()) != CHIAKI_ERR_SUCCESS)
			throw ChiakiException("Failed to initialize Raspberry Pi Decoder");
	}
	else
	{
#endif
		ffmpeg_decoder = new ChiakiFfmpegDecoder;
		ChiakiLogSniffer sniffer;
		chiaki_log_sniffer_init(&sniffer, CHIAKI_LOG_ALL, GetChiakiLog());
		err = chiaki_ffmpeg_decoder_init(ffmpeg_decoder,
				chiaki_log_sniffer_get_log(&sniffer),
				chiaki_target_is_ps5(connect_info.target) ? connect_info.video_profile.codec : CHIAKI_CODEC_H264,
				connect_info.hw_decoder.isEmpty() ? NULL : connect_info.hw_decoder.toUtf8().constData(),
				connect_info.hw_device_ctx, FfmpegFrameCb, this);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			QString log = QString::fromUtf8(chiaki_log_sniffer_get_buffer(&sniffer));
			chiaki_log_sniffer_fini(&sniffer);
			throw ChiakiException("Failed to initialize FFMPEG Decoder:\n" + log);
		}
		chiaki_log_sniffer_fini(&sniffer);
		ffmpeg_decoder->log = GetChiakiLog();
#if CHIAKI_LIB_ENABLE_PI_DECODER
	}
#endif

	audio_out_device_name = connect_info.audio_out_device;
	audio_in_device_name = connect_info.audio_in_device;

	chiaki_opus_decoder_init(&opus_decoder, log.GetChiakiLog());
	chiaki_opus_encoder_init(&opus_encoder, log.GetChiakiLog());
#if CHIAKI_GUI_ENABLE_SPEEX
	speech_processing_enabled = connect_info.speech_processing_enabled;
	if(speech_processing_enabled)
	{
		echo_state = speex_echo_state_init(MICROPHONE_SAMPLES, MICROPHONE_SAMPLES * 10);
		preprocess_state = speex_preprocess_state_init(MICROPHONE_SAMPLES, MICROPHONE_SAMPLES * 100);
		int32_t noise_suppress_level = -1 * connect_info.noise_suppress_level;
		int32_t echo_suppress_level = -1 * connect_info.echo_suppress_level;
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_ECHO_STATE, echo_state);
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_suppress_level);
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_GET_NOISE_SUPPRESS, &noise_suppress_level);
		CHIAKI_LOGI(GetChiakiLog(), "Noise suppress level is %i dB", noise_suppress_level);
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &echo_suppress_level);
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_GET_ECHO_SUPPRESS, &echo_suppress_level);
		CHIAKI_LOGI(GetChiakiLog(), "Echo suppress level is %i dB", echo_suppress_level);
		CHIAKI_LOGI(GetChiakiLog(), "Started microphone echo cancellation and noise suppression");
	}
#endif
	audio_buffer_size = connect_info.audio_buffer_size;

	host = connect_info.host;
	QByteArray host_str = connect_info.host.toUtf8();

	ChiakiConnectInfo chiaki_connect_info = {};
	chiaki_connect_info.ps5 = chiaki_target_is_ps5(connect_info.target);
	chiaki_connect_info.host = host_str.constData();
	chiaki_connect_info.video_profile = connect_info.video_profile;
	chiaki_connect_info.video_profile_auto_downgrade = true;
	chiaki_connect_info.enable_keyboard = false;
	chiaki_connect_info.enable_dualsense = connect_info.enable_dualsense;

#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(connect_info.decoder == Decoder::Pi && chiaki_connect_info.video_profile.codec != CHIAKI_CODEC_H264)
	{
		CHIAKI_LOGW(GetChiakiLog(), "A codec other than H264 was requested for Pi Decoder. Falling back to it.");
		chiaki_connect_info.video_profile.codec = CHIAKI_CODEC_H264;
	}
#endif

	if(connect_info.regist_key.size() != sizeof(chiaki_connect_info.regist_key))
		throw ChiakiException("RegistKey invalid");
	memcpy(chiaki_connect_info.regist_key, connect_info.regist_key.constData(), sizeof(chiaki_connect_info.regist_key));

	if(connect_info.morning.size() != sizeof(chiaki_connect_info.morning))
		throw ChiakiException("Morning invalid");
	memcpy(chiaki_connect_info.morning, connect_info.morning.constData(), sizeof(chiaki_connect_info.morning));

	if(chiaki_connect_info.ps5)
	{
		PS_TOUCHPAD_MAX_X = PS5_TOUCHPAD_MAX_X;
		PS_TOUCHPAD_MAX_Y = PS5_TOUCHPAD_MAX_Y;
	}
	else
	{
		PS_TOUCHPAD_MAX_X = PS4_TOUCHPAD_MAX_X;
		PS_TOUCHPAD_MAX_Y = PS4_TOUCHPAD_MAX_Y;
	}

	chiaki_controller_state_set_idle(&keyboard_state);
	chiaki_controller_state_set_idle(&touch_state);
	touch_tracker=QMap<int, uint8_t>();
	mouse_touch_id=-1;

	err = chiaki_session_init(&session, &chiaki_connect_info, GetChiakiLog());
	if(err != CHIAKI_ERR_SUCCESS)
		throw ChiakiException("Chiaki Session Init failed: " + QString::fromLocal8Bit(chiaki_error_string(err)));
	chiaki_opus_decoder_set_cb(&opus_decoder, AudioSettingsCb, AudioFrameCb, this);
	ChiakiAudioSink audio_sink;
	chiaki_opus_decoder_get_sink(&opus_decoder, &audio_sink);
	chiaki_session_set_audio_sink(&session, &audio_sink);
	ChiakiAudioHeader audio_header;
	chiaki_audio_header_set(&audio_header, 2, 16, MICROPHONE_SAMPLES * 100, MICROPHONE_SAMPLES);
	chiaki_opus_encoder_header(&audio_header, &opus_encoder, &session);

	if (connect_info.enable_dualsense)
	{
		ChiakiAudioSink haptics_sink;
		haptics_sink.user = this;
		haptics_sink.frame_cb = HapticsFrameCb;
		chiaki_session_set_haptics_sink(&session, &haptics_sink);
	}

#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(pi_decoder)
		chiaki_session_set_video_sample_cb(&session, chiaki_pi_decoder_video_sample_cb, pi_decoder);
	else
	{
#endif
		chiaki_session_set_video_sample_cb(&session, chiaki_ffmpeg_decoder_video_sample_cb, ffmpeg_decoder);
#if CHIAKI_LIB_ENABLE_PI_DECODER
	}
#endif

	chiaki_session_set_event_cb(&session, EventCb, this);

#if CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
	connect(ControllerManager::GetInstance(), &ControllerManager::AvailableControllersUpdated, this, &StreamSession::UpdateGamepads);
	if(connect_info.buttons_by_pos)
		ControllerManager::GetInstance()->SetButtonsByPos();
#endif
#if CHIAKI_GUI_ENABLE_SETSU
	setsu_motion_device = nullptr;
	chiaki_controller_state_set_idle(&setsu_state);
	setsu_ids=QMap<QPair<QString, SetsuTrackingId>, uint8_t>();
	orient_dirty = true;
	chiaki_orientation_tracker_init(&orient_tracker);
	setsu = setsu_new();
	auto timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this]{
		setsu_poll(setsu, SessionSetsuCb, this);
		if(orient_dirty)
		{
			chiaki_orientation_tracker_apply_to_controller_state(&orient_tracker, &setsu_state);
			SendFeedbackState();
			orient_dirty = false;
		}
	});
	timer->start(SETSU_UPDATE_INTERVAL_MS);
#endif

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	chiaki_controller_state_set_idle(&sdeck_state);
	sdeck = sdeck_new();
	// no concept of hotplug for Steam Deck so turn off if not detected immediately
	if(!sdeck)
		CHIAKI_LOGI(GetChiakiLog(), "Steam Deck not found ... Steam Deck native features disabled\n\n");
	else
	{
		CHIAKI_LOGI(GetChiakiLog(), "Connected Steam Deck ... gyro online\n\n");
		vertical_sdeck = connect_info.vertical_sdeck;
		if(vertical_sdeck)
		{
			chiaki_orientation_tracker_init(&sdeck_orient_tracker);
			sdeck_orient_dirty = true;
		}
		else
			sdeck_orient_dirty = false;
		auto sdeck_timer = new QTimer(this);
		connect(sdeck_timer, &QTimer::timeout, this, [this]{
			sdeck_read(sdeck, SessionSDeckCb, this);
			if(sdeck_orient_dirty)
			{
				chiaki_orientation_tracker_apply_to_controller_state(&sdeck_orient_tracker, &sdeck_state);
				SendFeedbackState();
				sdeck_orient_dirty = false;
			}
		});
		sdeck_timer->start(STEAMDECK_UPDATE_INTERVAL_MS);
	}
#endif
	key_map = connect_info.key_map;
	if (connect_info.enable_dualsense)
	{
		InitHaptics();
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		// Connect Steam Deck haptics with a delay to give other potential haptics time to set up
		if(sdeck)
			QTimer::singleShot(1100, this, &StreamSession::ConnectSdeckHaptics);
#endif
	}
	UpdateGamepads();
}

StreamSession::~StreamSession()
{
	if(audio_out)
		SDL_CloseAudioDevice(audio_out);
	if(audio_in)
		SDL_CloseAudioDevice(audio_in);
	chiaki_session_join(&session);
	chiaki_session_fini(&session);
	chiaki_opus_decoder_fini(&opus_decoder);
	chiaki_opus_encoder_fini(&opus_encoder);
#if CHIAKI_GUI_ENABLE_SPEEX
	if(speech_processing_enabled)
	{
		speex_echo_state_destroy(echo_state);
		speex_preprocess_state_destroy(preprocess_state);
	}
#endif
#if CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
	for(auto controller : controllers)
		delete controller;
#endif
#if CHIAKI_GUI_ENABLE_SETSU
	setsu_free(setsu);
#endif
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	sdeck_free(sdeck);
#endif
#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(pi_decoder)
	{
		chiaki_pi_decoder_fini(pi_decoder);
		free(pi_decoder);
	}
#endif
	if(ffmpeg_decoder)
	{
		chiaki_ffmpeg_decoder_fini(ffmpeg_decoder);
		delete ffmpeg_decoder;
	}
	if (haptics_output > 0)
	{
		SDL_CloseAudioDevice(haptics_output);
		haptics_output = 0;
	}
	if (haptics_resampler_buf)
	{
		free(haptics_resampler_buf);
		haptics_resampler_buf = nullptr;
	}
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	if (sdeck_haptics_senderl)
	{
		free(sdeck_haptics_senderl);
		sdeck_haptics_senderl = nullptr;
	}
	if (sdeck_haptics_senderr)
	{
		free(sdeck_haptics_senderr);
		sdeck_haptics_senderr = nullptr;
	}
	if(mic_buf.buf)
	{
		free(mic_buf.buf);
		mic_buf.buf = nullptr;
	}
#endif
#if CHIAKI_GUI_ENABLE_SPEEX
	if(speech_processing_enabled)
	{
		if(mic_resampler_buf)
		{
			free(mic_resampler_buf);
			mic_resampler_buf = nullptr;
		}
		if(echo_resampler_buf)
		{
			free(echo_resampler_buf);
			echo_resampler_buf = nullptr;
		}
	}
#endif
}

void StreamSession::Start()
{
	ChiakiErrorCode err = chiaki_session_start(&session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		chiaki_session_fini(&session);
		throw ChiakiException("Chiaki Session Start failed");
	}
}

void StreamSession::Stop()
{
	chiaki_session_stop(&session);
}

void StreamSession::GoToBed()
{
	chiaki_session_goto_bed(&session);
}

void StreamSession::ToggleMute()
{
	if(!allow_unmute)
		return;
	if(!mic_connected)
	{
#if CHIAKI_GUI_ENABLE_SPEEX
		if(speech_processing_enabled)
		{
			//Use 1 channel for SPEEX processing and then mix to 2 channels
			InitMic(1, opus_encoder.audio_header.rate);
		}
		else
			InitMic(2, opus_encoder.audio_header.rate);
#else
		InitMic(2, opus_encoder.audio_header.rate);
#endif
		chiaki_session_connect_microphone(&session);
		mic_connected = true;
	}
	chiaki_session_toggle_microphone(&session, muted);
	if (muted)
		muted = false;
	else
		muted = true;
	if(audio_in)
		SDL_PauseAudioDevice(audio_in, muted);
	emit MutedChanged();
}

void StreamSession::SetLoginPIN(const QString &pin)
{
	QByteArray data = pin.toUtf8();
	chiaki_session_set_login_pin(&session, (const uint8_t *)data.constData(), data.size());
}

void StreamSession::HandleMousePressEvent(QMouseEvent *event)
{
	// left button for touchpad gestures, others => touchpad click
	if (event->button() != Qt::MouseButton::LeftButton)
		keyboard_state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
	SendFeedbackState();
}

void StreamSession::HandleMouseReleaseEvent(QMouseEvent *event)
{
	// left button => end of touchpad gesture
	if (event->button() == Qt::LeftButton)
	{
		if(mouse_touch_id >= 0)
		{
			chiaki_controller_state_stop_touch(&keyboard_state, (uint8_t)mouse_touch_id);
			mouse_touch_id = -1;
		}
	}
	keyboard_state.buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
	SendFeedbackState();
}

void StreamSession::HandleMouseMoveEvent(QMouseEvent *event, float width, float height)
{
	// left button with move => touchpad gesture, otherwise ignore
	if (event->buttons() == Qt::LeftButton)
	{
		float x = event->screenPos().x();
		float y = event->screenPos().y();
		float psx = x * (PS_TOUCHPAD_MAX_X / width);
		float psy = y * (PS_TOUCHPAD_MAX_Y / height);
		// if touch id is set, move, otherwise start
		if(mouse_touch_id >= 0)
			chiaki_controller_state_set_touch_pos(&keyboard_state, (uint8_t)mouse_touch_id, (uint16_t)psx, (uint16_t)psy);
		else
			mouse_touch_id=chiaki_controller_state_start_touch(&keyboard_state, (uint16_t)psx, (uint16_t)psy);	
	}
	SendFeedbackState();
}

void StreamSession::HandleKeyboardEvent(QKeyEvent *event)
{
	if(key_map.contains(Qt::Key(event->key())) == false)
		return;

	if(event->isAutoRepeat())
		return;

	int button = key_map[Qt::Key(event->key())];
	bool press_event = event->type() == QEvent::Type::KeyPress;

	switch(button)
	{
		case CHIAKI_CONTROLLER_ANALOG_BUTTON_L2:
			keyboard_state.l2_state = press_event ? 0xff : 0;
			break;
		case CHIAKI_CONTROLLER_ANALOG_BUTTON_R2:
			keyboard_state.r2_state = press_event ? 0xff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_Y_UP):
			keyboard_state.right_y = press_event ? -0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_Y_DOWN):
			keyboard_state.right_y = press_event ? 0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_X_UP):
			keyboard_state.right_x = press_event ? 0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_X_DOWN):
			keyboard_state.right_x = press_event ? -0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_Y_UP):
			keyboard_state.left_y = press_event ? -0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_Y_DOWN):
			keyboard_state.left_y = press_event ? 0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_X_UP):
			keyboard_state.left_x = press_event ? 0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_X_DOWN):
			keyboard_state.left_x = press_event ? -0x7fff : 0;
			break;
		default:
			if(press_event)
				keyboard_state.buttons |= button;
			else
				keyboard_state.buttons &= ~button;
			break;
	}

	SendFeedbackState();
}

void StreamSession::HandleTouchEvent(QTouchEvent *event)
{
	//unset touchpad (we will set it if user touches edge of screen)
	touch_state.buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;

	const QList<QTouchEvent::TouchPoint> touchPoints = event->touchPoints();

	for (const QTouchEvent::TouchPoint &touchPoint : touchPoints)
	{
		int id = touchPoint.id();
		switch (touchPoint.state())
		{
			//skip unchanged touchpoints
			case Qt::TouchPointStationary:
				continue;
			case Qt::TouchPointPressed:
			case Qt::TouchPointMoved:
			{
				float norm_x = touchPoint.normalizedPos().x();
				float norm_y = touchPoint.normalizedPos().y();
				
				// Touching edges of screen is a touchpad click
				if(norm_x <= 0.05 || norm_x >= 0.95 || norm_y <= 0.05 || norm_y >= 0.95)
					touch_state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
				// Scale to PS TouchPad since that's what PS Console expects
				float psx = norm_x * PS_TOUCHPAD_MAX_X;
				float psy = norm_y * PS_TOUCHPAD_MAX_Y;
				auto it = touch_tracker.find(id);
				if(it == touch_tracker.end())
				{
					int8_t cid = chiaki_controller_state_start_touch(&touch_state, (uint16_t)psx, (uint16_t)psy);
					// if cid < 0 => already too many multi-touches
					if(cid >= 0)
						touch_tracker[id] = (uint8_t)cid;
					else
						break;
				}
				else
					chiaki_controller_state_set_touch_pos(&touch_state, it.value(), (uint16_t)psx, (uint16_t)psy);
				break;
			}
			case Qt::TouchPointReleased:
			{
				for(auto it=touch_tracker.begin(); it!=touch_tracker.end(); it++)
				{
					if(it.key() == id)
					{
						chiaki_controller_state_stop_touch(&touch_state, it.value());
						touch_tracker.erase(it);
						break;
					}
				}
				break;
			}
		}
	}
	SendFeedbackState();
}

void StreamSession::UpdateGamepads()
{
#if CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
	for(auto controller_id : controllers.keys())
	{
		auto controller = controllers[controller_id];
		if(!controller->IsConnected())
		{
			CHIAKI_LOGI(log.GetChiakiLog(), "Controller %d disconnected", controller->GetDeviceID());
			controllers.remove(controller_id);
			if (controller->IsDualSense())
			{
				DisconnectHaptics();
			}
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
			if (!controller->IsSteamDeck())
			{
				haptics_sdeck++;
			}
#endif
			delete controller;
		}
	}

	const auto available_controllers = ControllerManager::GetInstance()->GetAvailableControllers();
	for(auto controller_id : available_controllers)
	{
		if(!controllers.contains(controller_id))
		{
			auto controller = ControllerManager::GetInstance()->OpenController(controller_id);
			if(!controller)
			{
				CHIAKI_LOGE(log.GetChiakiLog(), "Failed to open controller %d", controller_id);
				continue;
			}
			CHIAKI_LOGI(log.GetChiakiLog(), "Controller %d opened: \"%s\"", controller_id, controller->GetName().toLocal8Bit().constData());
			connect(controller, &Controller::StateChanged, this, &StreamSession::SendFeedbackState);
			connect(controller, &Controller::MicButtonPush, this, &StreamSession::ToggleMute);
			controllers[controller_id] = controller;
			if (controller->IsDualSense())
			{
				// Connect haptics audio device with a delay to give the sound system time to set up
				QTimer::singleShot(1000, this, &StreamSession::ConnectHaptics);
			}
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
			if (!controller->IsSteamDeck())
			{
				haptics_sdeck--;
			}
#endif
		}
	}
	
	SendFeedbackState();
#endif
}

void StreamSession::SendFeedbackState()
{
	ChiakiControllerState state;
	chiaki_controller_state_set_idle(&state);

	if(input_blocked)
	{
		chiaki_controller_state_set_idle(&keyboard_state);
		chiaki_session_set_controller_state(&session, &state);
		return;
	}

#if CHIAKI_GUI_ENABLE_SETSU
	// setsu is the one that potentially has gyro/accel/orient so copy that directly first
	state = setsu_state;
#endif

	for(auto controller : controllers)
	{
		auto controller_state = controller->GetState();
		chiaki_controller_state_or(&state, &state, &controller_state);
	}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	chiaki_controller_state_or(&state, &state, &sdeck_state);
#endif
	chiaki_controller_state_or(&state, &state, &keyboard_state);
	chiaki_controller_state_or(&state, &state, &touch_state);
	chiaki_session_set_controller_state(&session, &state);
}

void StreamSession::InitAudio(unsigned int channels, unsigned int rate)
{
	if(audio_out)
		SDL_CloseAudioDevice(audio_out);

	SDL_AudioSpec spec = {0};
	spec.freq = rate;
	spec.channels = channels;
	spec.format = AUDIO_S16SYS;
	audio_out_sample_size = sizeof(int16_t) * channels;
	spec.samples = audio_buffer_size / audio_out_sample_size;

	SDL_AudioSpec obtained;
	audio_out = SDL_OpenAudioDevice(audio_out_device_name.isEmpty() ? nullptr : qPrintable(audio_out_device_name), false, &spec, &obtained, false);
	if(!audio_out)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Failed to open Audio Output Device: %s", SDL_GetError());
		return;
	}

	SDL_PauseAudioDevice(audio_out, 0);

	CHIAKI_LOGI(log.GetChiakiLog(), "Audio Device %s opened with %u channels @ %d Hz, buffer size %u",
				qPrintable(audio_out_device_name), obtained.channels, obtained.freq, obtained.size);
	allow_unmute = true;
}

void StreamSession::InitMic(unsigned int channels, unsigned int rate)
{
	if(audio_in)
		SDL_CloseAudioDevice(audio_in);

	mic_buf.buf = nullptr;
#if CHIAKI_GUI_ENABLE_SPEEX
	mic_resampler_buf = nullptr;
	echo_resampler_buf = nullptr;
#endif

	mic_buf.current_byte = 0;
	int16_t mic_buf_size = channels * MICROPHONE_SAMPLES;
	mic_buf.size_bytes = mic_buf_size * sizeof(int16_t);
	mic_buf.buf = (int16_t*) calloc(mic_buf_size, sizeof(int16_t));

#if CHIAKI_GUI_ENABLE_SPEEX
	if(speech_processing_enabled)
	{
		SDL_AudioCVT cvt;
		SDL_BuildAudioCVT(&cvt, AUDIO_S16LSB, 1, 48000, AUDIO_S16LSB, 2, 48000);
		cvt.len = mic_buf.size_bytes;
		mic_resampler_buf = (uint8_t*) calloc(cvt.len * cvt.len_mult, sizeof(uint8_t));
		if(!mic_resampler_buf)
		{
			CHIAKI_LOGE(GetChiakiLog(), "Mic resampler buf could not be created, aborting mic startup");
			return;
		}

		SDL_AudioCVT cvt2;
		SDL_BuildAudioCVT(&cvt2, AUDIO_S16LSB, 2, 48000, AUDIO_S16LSB, 1, 48000);
		cvt2.len = cvt.len * cvt.len_ratio;
		echo_resampler_buf = (uint8_t*) calloc(cvt2.len * cvt2.len_mult, sizeof(uint8_t));
		if(!echo_resampler_buf)
		{
			CHIAKI_LOGE(GetChiakiLog(), "Echo resampler buf could not be created, aborting mic startup");
			return;
		}
	}
#endif

	SDL_AudioSpec spec = {0};
	spec.freq = rate;
	spec.channels = channels;
	spec.format = AUDIO_S16SYS;
	spec.samples = audio_buffer_size / 4;
	spec.callback = [](void *userdata, Uint8 *stream, int len) {
		auto s = static_cast<StreamSession*>(userdata);
		QByteArray data(reinterpret_cast<char*>(stream), len);
		QMetaObject::invokeMethod(s, std::bind(&StreamSession::ReadMic, s, data));
	};
	spec.userdata = this;

	SDL_AudioSpec obtained;
	audio_in = SDL_OpenAudioDevice(audio_in_device_name.isEmpty() ? nullptr : qPrintable(audio_in_device_name), true, &spec, &obtained, false);
	if(!audio_in)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Failed to open Audio Input Device: %s", SDL_GetError());
		return;
	}

	CHIAKI_LOGI(log.GetChiakiLog(), "Microphone %s opened with %u channels @ %u Hz, buffer size %u",
			qPrintable(audio_in_device_name), obtained.channels, obtained.freq, obtained.size);
}

void StreamSession::ReadMic(const QByteArray &micdata)
{
#if CHIAKI_GUI_ENABLE_SPEEX
	int16_t echo_buf[mic_buf.size_bytes / sizeof(int16_t)];
#endif
	uint32_t mic_bytes_left = mic_buf.size_bytes - mic_buf.current_byte;
	// Don't send mic data if muted
	if(muted)
		return;
	qint64 bytes_read = micdata.size();
	const char * micdataread = micdata.constData();
	if(bytes_read == 0)
		return;
	if(bytes_read < mic_bytes_left)
	{
		memcpy((uint8_t *)mic_buf.buf + mic_buf.current_byte, (uint8_t *)micdataread, bytes_read);
		mic_buf.current_byte += bytes_read;
	}
	else
	{
		memcpy((uint8_t *)mic_buf.buf + mic_buf.current_byte, (uint8_t *)micdataread, mic_bytes_left);
#if CHIAKI_GUI_ENABLE_SPEEX
		SDL_AudioCVT cvt;
		if(speech_processing_enabled)
		{
			// change samples to stereo after processing with SPEEX
			SDL_BuildAudioCVT(&cvt, AUDIO_S16LSB, 1, 48000, AUDIO_S16LSB, 2, 48000);
			cvt.len = mic_buf.size_bytes;
			cvt.buf = mic_resampler_buf;
			if(!echo_to_cancel.isEmpty())
			{
				int16_t *echo = echo_to_cancel.dequeue();
				speex_echo_cancellation(echo_state, mic_buf.buf, echo, echo_buf);
				speex_preprocess_run(preprocess_state, echo_buf);
				memcpy((uint8_t *)mic_resampler_buf, (uint8_t *)echo_buf, mic_buf.size_bytes);
				if(SDL_ConvertAudio(&cvt) != 0)
				{
					CHIAKI_LOGE(log.GetChiakiLog(), "Failed to resample mic audio: %s", SDL_GetError());
					return;
				}
				chiaki_opus_encoder_frame((int16_t *)mic_resampler_buf, &opus_encoder);
			}
			else
			{
				memcpy((uint8_t *)mic_buf.buf, (uint8_t *)mic_buf.buf, mic_buf.size_bytes);
				speex_preprocess_run(preprocess_state, (int16_t *)mic_buf.buf);
				memcpy((uint8_t *)mic_resampler_buf, (uint8_t *)mic_buf.buf, mic_buf.size_bytes);
				if(SDL_ConvertAudio(&cvt) != 0)
				{
					CHIAKI_LOGE(log.GetChiakiLog(), "Failed to resample mic audio: %s", SDL_GetError());
					return;
				}
				chiaki_opus_encoder_frame((int16_t *)mic_resampler_buf, &opus_encoder);
			}
		}
		else
			chiaki_opus_encoder_frame(mic_buf.buf, &opus_encoder);
#else
	    chiaki_opus_encoder_frame(mic_buf.buf, &opus_encoder);
#endif
		bytes_read -= mic_bytes_left;
		uint32_t frames = bytes_read / mic_buf.size_bytes;
		for (int i = 0; i < frames; i++)
		{
			memcpy((uint8_t *)mic_buf.buf, (uint8_t *)micdataread + mic_bytes_left + i * mic_buf.size_bytes, mic_buf.size_bytes);
#if CHIAKI_GUI_ENABLE_SPEEX
		if(speech_processing_enabled)
		{
			if(!echo_to_cancel.isEmpty())
			{
				int16_t *echo = echo_to_cancel.dequeue();
				speex_echo_cancellation(echo_state, mic_buf.buf, echo, echo_buf);
				speex_preprocess_run(preprocess_state, echo_buf);
				memcpy((uint8_t *)mic_resampler_buf, (uint8_t *)echo_buf, mic_buf.size_bytes);
				if(SDL_ConvertAudio(&cvt) != 0)
				{
					CHIAKI_LOGE(log.GetChiakiLog(), "Failed to resample mic audio: %s", SDL_GetError());
					return;
				}
				chiaki_opus_encoder_frame((int16_t *)mic_resampler_buf, &opus_encoder);
			}
			else
			{
				memcpy((uint8_t *)mic_buf.buf, (uint8_t *)mic_buf.buf, mic_buf.size_bytes);
				speex_preprocess_run(preprocess_state, (int16_t *)mic_buf.buf);
				memcpy((uint8_t *)mic_resampler_buf, (uint8_t *)mic_buf.buf, mic_buf.size_bytes);
				if(SDL_ConvertAudio(&cvt) != 0)
				{
					CHIAKI_LOGE(log.GetChiakiLog(), "Failed to resample mic audio: %s", SDL_GetError());
					return;
				}
				chiaki_opus_encoder_frame((int16_t *)mic_resampler_buf, &opus_encoder);
			}
		}
		else
			chiaki_opus_encoder_frame(mic_buf.buf, &opus_encoder);
#else
	    chiaki_opus_encoder_frame(mic_buf.buf, &opus_encoder);
#endif
		}
		mic_buf.current_byte = bytes_read % mic_buf.size_bytes;
		if(mic_buf.current_byte == 0)
			return;
		memcpy((uint8_t *)mic_buf.buf, (uint8_t *)micdataread + mic_bytes_left + frames * mic_buf.size_bytes, mic_buf.current_byte);
	}
}
void StreamSession::InitHaptics()
{
	haptics_output = 0;
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	sdeck_haptics_senderr = nullptr;
	sdeck_haptics_senderl = nullptr;
#endif
	haptics_resampler_buf = nullptr;
#ifdef Q_OS_LINUX
	// Haptics work most reliably with Pipewire, so try to use that if available
	SDL_SetHint("SDL_AUDIODRIVER", "pipewire");
#endif

#ifdef Q_OS_LINUX
	if (!strstr(SDL_GetCurrentAudioDriver(), "pipewire"))
	{
		CHIAKI_LOGW(
			log.GetChiakiLog(),
			"Haptics output is not using Pipewire, this may not work reliably. (was: '%s')",
			SDL_GetCurrentAudioDriver());
	}
#endif

	SDL_AudioCVT cvt;
	SDL_BuildAudioCVT(&cvt, AUDIO_S16LSB, 4, 3000, AUDIO_S16LSB, 4, 48000);
	cvt.len = 240;  // 10 16bit stereo samples
	haptics_resampler_buf = (uint8_t*) calloc(cvt.len * cvt.len_mult, sizeof(uint8_t));
}

void StreamSession::DisconnectHaptics()
{
	if (this->haptics_output > 0)
	{
		SDL_CloseAudioDevice(haptics_output);
		this->haptics_output = 0;
	}
}

void StreamSession::ConnectHaptics()
{
	if (this->haptics_output > 0)
	{
		CHIAKI_LOGW(this->log.GetChiakiLog(), "Haptics already connected to an attached DualSense controller, ignoring additional controllers.");
		return;
	}

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 48000;
	want.format = AUDIO_S16LSB;
	want.channels = 4;
	want.samples = 480; // 10ms buffer
	want.callback = NULL;

	const char *device_name = nullptr;
	for (int i=0; i < SDL_GetNumAudioDevices(0); i++)
	{
		device_name = SDL_GetAudioDeviceName(i, 0);
		if (!device_name || !strstr(device_name, DUALSENSE_AUDIO_DEVICE_NEEDLE))
		{
			continue;
		}
		haptics_output = SDL_OpenAudioDevice(device_name, 0, &want, &have, 0);
		if (haptics_output == 0)
		{
			CHIAKI_LOGE(log.GetChiakiLog(), "Could not open SDL Audio Device %s for haptics output: %s", device_name, SDL_GetError());
			continue;
		}
		SDL_PauseAudioDevice(haptics_output, 0);
		CHIAKI_LOGI(log.GetChiakiLog(), "Haptics Audio Device '%s' opened with %d channels @ %d Hz, buffer size %u (driver=%s)", device_name, have.channels, have.freq, have.size, SDL_GetCurrentAudioDriver());
		return;
	}
	CHIAKI_LOGW(log.GetChiakiLog(), "DualSense features were enabled and a DualSense is connected, but could not find the DualSense audio device!");
	return;
}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
void StreamSession::ConnectSdeckHaptics()
{
	haptics_sdeck++;
	sdeck_last_haptic = chiaki_time_now_monotonic_ms();
	const int num_channels = 2; // Left and right haptics
	const uint32_t samples_per_packet = 120 * sizeof(uint8_t) / (2.0 * sizeof(int16_t));
	sdeck_queue_segment = samples_per_packet * STEAMDECK_HAPTIC_PACKETS_PER_ANALYSIS;
	if (sdeck_haptic_init(sdeck, sdeck_queue_segment) < 0)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Steam Deck Haptics Audio could not be connected :(");
		return;
	}
	CHIAKI_LOGI(log.GetChiakiLog(), "Steam Deck Haptics Audio opened with %d channels @ %d Hz with %u samples per audio analysis.", num_channels, STEAMDECK_HAPTIC_SAMPLING_RATE, sdeck_queue_segment);
	sdeck_hapticl = {};
	sdeck_hapticl.reserve(20);
	sdeck_hapticr = {};
	sdeck_hapticr.reserve(20);
	sdeck_skipl = false;
	sdeck_skipr = false;
	sdeck_haptics_senderl = (int16_t *) calloc(sdeck_queue_segment, sizeof(uint16_t));
	sdeck_haptics_senderr = (int16_t *) calloc(sdeck_queue_segment, sizeof(uint16_t));

	qRegisterMetaType<haptic_packet_t>();
	connect(this, &StreamSession::SdeckHapticPushed, this, &StreamSession::SdeckQueueHaptics);
	auto sdeck_haptic_interval = STEAMDECK_HAPTIC_PACKETS_PER_ANALYSIS * 10;
	auto sdeck_haptic_timer = new QTimer(this);
	connect(sdeck_haptic_timer, &QTimer::timeout, this, [this]{
		haptic_packet_t haptic_packetl = {}, haptic_packetr = {};
		bool changedl = false, changedr = false;
		for(uint64_t i = 0; i < STEAMDECK_HAPTIC_PACKETS_PER_ANALYSIS; i++)
		{
			uint64_t current_tick = sdeck_last_haptic + i * 10; // 10ms packet
			if(!sdeck_hapticl.isEmpty())
			{
				uint64_t next_timestamp = sdeck_hapticl.head().timestamp;
				if(next_timestamp > (current_tick + 10))
				{
					memset(sdeck_haptics_senderl + 30 * i, 0, 30);
					continue;
				}
				else
				{
					haptic_packetl = sdeck_hapticl.dequeue();
					memcpy(sdeck_haptics_senderl + 30 * i, haptic_packetl.haptic_packet, 30);
					changedl = true;
				}
			}
			else
				memset(sdeck_haptics_senderl + 30 * i, 0, 30);

			if(!sdeck_hapticr.isEmpty())
			{
				uint64_t next_timestamp = sdeck_hapticr.head().timestamp;
				if(next_timestamp > (current_tick + 10))
				{
					memset(sdeck_haptics_senderr + 30 * i, 0, 30);
					continue;
				}
				else
				{
					haptic_packetr = sdeck_hapticr.dequeue();
					memcpy(sdeck_haptics_senderr + 30 * i, haptic_packetr.haptic_packet, 30);
					changedr = true;
				}
			}
			else
				memset(sdeck_haptics_senderr + 30 * i, 0, 30);
		}

		if(!changedl || sdeck_skipl)
			sdeck_skipl = false;
		else
		{
			int intervals = play_pcm_haptic(sdeck, TRACKPAD_LEFT, sdeck_haptics_senderl, sdeck_queue_segment, STEAMDECK_HAPTIC_SAMPLING_RATE);
			if (intervals < 0)
				CHIAKI_LOGE(log.GetChiakiLog(), "Failed to submit haptics audio to SteamDeck");
			else if (intervals == 2)
				sdeck_skipl = true;
		}
		if(!changedr || sdeck_skipr)
			sdeck_skipr = false;
		else
		{
			int intervals = play_pcm_haptic(sdeck, TRACKPAD_RIGHT, sdeck_haptics_senderr, sdeck_queue_segment, STEAMDECK_HAPTIC_SAMPLING_RATE);
			if (intervals < 0)
				CHIAKI_LOGE(log.GetChiakiLog(), "Failed to submit haptics audio to SteamDeck");
			else if (intervals == 2)
				sdeck_skipr = true;
		}
		sdeck_last_haptic = chiaki_time_now_monotonic_ms();
	});
	sdeck_haptic_timer->start(sdeck_haptic_interval);
	return;
}
#endif

void StreamSession::PushAudioFrame(int16_t *buf, size_t samples_count)
{
	if(!audio_out)
		return;

#if CHIAKI_GUI_ENABLE_SPEEX
	// change samples to mono for processing with SPEEX
	if(echo_resampler_buf && speech_processing_enabled && !muted)
	{
		SDL_AudioCVT cvt;
		SDL_BuildAudioCVT(&cvt, AUDIO_S16LSB, 2, 48000, AUDIO_S16LSB, 1, 48000);
		cvt.len = mic_buf.size_bytes * 2;
		cvt.buf = echo_resampler_buf;
		memcpy(echo_resampler_buf, buf, mic_buf.size_bytes * 2);
		if(SDL_ConvertAudio(&cvt) != 0)
		{
			CHIAKI_LOGE(log.GetChiakiLog(), "Failed to resample echo audio: %s", SDL_GetError());
			return;
		}
		if(echo_to_cancel.size() >= ECHO_QUEUE_MAX)
			echo_to_cancel.dequeue();
		echo_to_cancel.enqueue((int16_t *)echo_resampler_buf);
	}
#endif
	SDL_QueueAudio(audio_out, buf, samples_count * audio_out_sample_size);
}

void StreamSession::PushHapticsFrame(uint8_t *buf, size_t buf_size)
{
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	if(sdeck && haptics_sdeck > 0)
	{
		if(buf_size != 120)
		{
			CHIAKI_LOGE(log.GetChiakiLog(), "Haptic audio of incompatible size: %u", buf_size);
			return;
		}
		int16_t amplitudel = 0, amplituder = 0;
		const size_t sample_size = 2 * sizeof(int16_t); // stereo samples
		haptic_packet_t packetl = {0}, packetr = {0};
		uint64_t timestamp = chiaki_time_now_monotonic_ms();
		packetl.timestamp = timestamp;
		packetr.timestamp = timestamp;
		size_t buf_count = buf_size / sample_size;
		for (size_t i = 0; i < buf_count; i++)
		{
			size_t cur = i * sample_size;
			memcpy(&amplitudel, buf + cur, sizeof(int16_t));
			packetl.haptic_packet[i] = amplitudel;
			memcpy(&amplituder, buf + cur + sizeof(int16_t), sizeof(int16_t));
			packetr.haptic_packet[i] = amplituder;
		}
		emit SdeckHapticPushed(packetl, packetr);
		return;
	}
#endif
	if(haptics_output == 0)
		return;
	SDL_AudioCVT cvt;
	// Haptics samples are coming in at 3KHZ, but the DualSense expects 48KHZ
	SDL_BuildAudioCVT(&cvt, AUDIO_S16LSB, 4, 3000, AUDIO_S16LSB, 4, 48000);
	cvt.len = buf_size * 2;
	cvt.buf = haptics_resampler_buf;
	// Remix to 4 channels
	for (int i=0; i < buf_size; i+=4)
	{
		SDL_memset(haptics_resampler_buf + i * 2, 0, 4);
		SDL_memcpy(haptics_resampler_buf + (i * 2) + 4, buf + i, 4);
	}
	// Resample to 48kHZ
	if (SDL_ConvertAudio(&cvt) != 0)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Failed to resample haptics audio: %s", SDL_GetError());
		return;
	}

	if (SDL_QueueAudio(haptics_output, cvt.buf, cvt.len_cvt) < 0)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Failed to submit haptics audio to device: %s", SDL_GetError());
		return;
	}
}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
void StreamSession::SdeckQueueHaptics(haptic_packet_t packetl, haptic_packet_t packetr)
{
	sdeck_hapticl.enqueue(packetl);
	sdeck_hapticr.enqueue(packetr);
}
#endif

void StreamSession::Event(ChiakiEvent *event)
{
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			connected = true;
			break;
		case CHIAKI_EVENT_QUIT:
			connected = false;
			emit SessionQuit(event->quit.reason, event->quit.reason_str ? QString::fromUtf8(event->quit.reason_str) : QString());
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			emit LoginPINRequested(event->login_pin_request.pin_incorrect);
			break;
		case CHIAKI_EVENT_RUMBLE: {
			uint8_t left = event->rumble.left;
			uint8_t right = event->rumble.right;
			QMetaObject::invokeMethod(this, [this, left, right]() {
				for(auto controller : controllers)
					controller->SetRumble(left, right);
			});
			break;
		}
		case CHIAKI_EVENT_TRIGGER_EFFECTS: {
			uint8_t type_left = event->trigger_effects.type_left;
			uint8_t data_left[10];
			memcpy(data_left, event->trigger_effects.left, 10);
			uint8_t data_right[10];
			memcpy(data_right, event->trigger_effects.right, 10);
			uint8_t type_right = event->trigger_effects.type_right;
			QMetaObject::invokeMethod(this, [this, type_left, data_left, type_right, data_right]() {
				for(auto controller : controllers)
					controller->SetTriggerEffects(type_left, data_left, type_right, data_right);
			});
			break;
		}
		default:
			break;
	}
}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
void StreamSession::HandleSDeckEvent(SDeckEvent *event)
{
	if(!sdeck)
	{
		CHIAKI_LOGI(GetChiakiLog(), "Steam Deck was disconnected! Skipping stale events...\n");
		return;
	}
	// right now only one event, use switch here for more in the future
	switch(event->type)
	{
		case SDECK_EVENT_MOTION:
			if(!vertical_sdeck)
			{
				sdeck_state.gyro_x = event->motion.gyro_x;
				sdeck_state.gyro_y = event->motion.gyro_y;
				sdeck_state.gyro_z = event->motion.gyro_z;
				sdeck_state.accel_x = event->motion.accel_x;
				sdeck_state.accel_y = event->motion.accel_y;
				sdeck_state.accel_z = event->motion.accel_z;
				sdeck_state.orient_w = event->motion.orient_w;
				sdeck_state.orient_x = event->motion.orient_x;
				sdeck_state.orient_y = event->motion.orient_y;
				sdeck_state.orient_z = event->motion.orient_z;
				SendFeedbackState();
			}
			else // swap y with z axis to use roll instead of yaw
			{
				// use calculated orient
				chiaki_orientation_tracker_update(&sdeck_orient_tracker,
					event->motion.gyro_x, -event->motion.gyro_z, event->motion.gyro_y,
					event->motion.accel_x, -event->motion.accel_z, event->motion.accel_y,
					chiaki_time_now_monotonic_us());
				sdeck_orient_dirty = true;
			}
			break;
	}
}
#endif

#if CHIAKI_GUI_ENABLE_SETSU
void StreamSession::HandleSetsuEvent(SetsuEvent *event)
{
	if(!setsu)
		return;
	switch(event->type)
	{
		case SETSU_EVENT_DEVICE_ADDED:
			switch(event->dev_type)
			{
				case SETSU_DEVICE_TYPE_TOUCHPAD:
					// connect all the touchpads!
					if(setsu_connect(setsu, event->path, event->dev_type))
						CHIAKI_LOGI(GetChiakiLog(), "Connected Setsu Touchpad Device %s", event->path);
					else
						CHIAKI_LOGE(GetChiakiLog(), "Failed to connect to Setsu Touchpad Device %s", event->path);
					break;
				case SETSU_DEVICE_TYPE_MOTION:
					// connect only one motion since multiple make no sense
					if(setsu_motion_device)
					{
						CHIAKI_LOGI(GetChiakiLog(), "Setsu Motion Device %s detected there is already one connected",
								event->path);
						break;
					}
					setsu_motion_device = setsu_connect(setsu, event->path, event->dev_type);
					if(setsu_motion_device)
						CHIAKI_LOGI(GetChiakiLog(), "Connected Setsu Motion Device %s", event->path);
					else
						CHIAKI_LOGE(GetChiakiLog(), "Failed to connect to Setsu Motion Device %s", event->path);
					break;
			}
			break;
		case SETSU_EVENT_DEVICE_REMOVED:
			switch(event->dev_type)
			{
				case SETSU_DEVICE_TYPE_TOUCHPAD:
					CHIAKI_LOGI(GetChiakiLog(), "Setsu Touchpad Device %s disconnected", event->path);
					for(auto it=setsu_ids.begin(); it!=setsu_ids.end();)
					{
						if(it.key().first == event->path)
						{
							chiaki_controller_state_stop_touch(&setsu_state, it.value());
							setsu_ids.erase(it++);
						}
						else
							it++;
					}
					SendFeedbackState();
					break;
				case SETSU_DEVICE_TYPE_MOTION:
					if(!setsu_motion_device || strcmp(setsu_device_get_path(setsu_motion_device), event->path))
						break;
					CHIAKI_LOGI(GetChiakiLog(), "Setsu Motion Device %s disconnected", event->path);
					setsu_motion_device = nullptr;
					chiaki_orientation_tracker_init(&orient_tracker);
					orient_dirty = true;
					break;
			}
			break;
		case SETSU_EVENT_TOUCH_DOWN:
			break;
		case SETSU_EVENT_TOUCH_UP:
			for(auto it=setsu_ids.begin(); it!=setsu_ids.end(); it++)
			{
				if(it.key().first == setsu_device_get_path(event->dev) && it.key().second == event->touch.tracking_id)
				{
					chiaki_controller_state_stop_touch(&setsu_state, it.value());
					setsu_ids.erase(it);
					break;
				}
			}
			SendFeedbackState();
			break;
		case SETSU_EVENT_TOUCH_POSITION: {
			QPair<QString, SetsuTrackingId> k =  { setsu_device_get_path(event->dev), event->touch.tracking_id };
			auto it = setsu_ids.find(k);
			if(it == setsu_ids.end())
			{
				int8_t cid = chiaki_controller_state_start_touch(&setsu_state, event->touch.x, event->touch.y);
				if(cid >= 0)
					setsu_ids[k] = (uint8_t)cid;
				else
					break;
			}
			else
				chiaki_controller_state_set_touch_pos(&setsu_state, it.value(), event->touch.x, event->touch.y);
			SendFeedbackState();
			break;
		}
		case SETSU_EVENT_BUTTON_DOWN:
			setsu_state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
			break;
		case SETSU_EVENT_BUTTON_UP:
			setsu_state.buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
			break;
		case SETSU_EVENT_MOTION:
			chiaki_orientation_tracker_update(&orient_tracker,
					event->motion.gyro_x, event->motion.gyro_y, event->motion.gyro_z,
					event->motion.accel_x, event->motion.accel_y, event->motion.accel_z,
					event->motion.timestamp);
			orient_dirty = true;
			break;
	}
}
#endif

void StreamSession::TriggerFfmpegFrameAvailable()
{
	emit FfmpegFrameAvailable();
	if(measured_bitrate != session.stream_connection.measured_bitrate)
	{
		measured_bitrate = session.stream_connection.measured_bitrate;
		emit MeasuredBitrateChanged();
	}
}

class StreamSessionPrivate
{
	public:
		static void InitAudio(StreamSession *session, uint32_t channels, uint32_t rate)
		{
			QMetaObject::invokeMethod(session, "InitAudio", Qt::ConnectionType::BlockingQueuedConnection, Q_ARG(unsigned int, channels), Q_ARG(unsigned int, rate));
		}

		static void InitMic(StreamSession *session, uint32_t channels, uint32_t rate)
		{
			QMetaObject::invokeMethod(session, "InitMic", Qt::ConnectionType::QueuedConnection, Q_ARG(unsigned int, channels), Q_ARG(unsigned int, rate));
		}

		static void PushAudioFrame(StreamSession *session, int16_t *buf, size_t samples_count)	{ session->PushAudioFrame(buf, samples_count); }
		static void PushHapticsFrame(StreamSession *session, uint8_t *buf, size_t buf_size)	{ session->PushHapticsFrame(buf, buf_size); }
		static void Event(StreamSession *session, ChiakiEvent *event)							{ session->Event(event); }
#if CHIAKI_GUI_ENABLE_SETSU
		static void HandleSetsuEvent(StreamSession *session, SetsuEvent *event)					{ session->HandleSetsuEvent(event); }
#endif
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		static void HandleSDeckEvent(StreamSession *session, SDeckEvent *event)					{ session->HandleSDeckEvent(event); }
#endif
		static void TriggerFfmpegFrameAvailable(StreamSession *session)							{ session->TriggerFfmpegFrameAvailable(); }
};

static void AudioSettingsCb(uint32_t channels, uint32_t rate, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::InitAudio(session, channels, rate);
}

static void AudioFrameCb(int16_t *buf, size_t samples_count, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::PushAudioFrame(session, buf, samples_count);
}

static void HapticsFrameCb(uint8_t *buf, size_t buf_size, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::PushHapticsFrame(session, buf, buf_size);
}

static void EventCb(ChiakiEvent *event, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::Event(session, event);
}

#if CHIAKI_GUI_ENABLE_SETSU
static void SessionSetsuCb(SetsuEvent *event, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::HandleSetsuEvent(session, event);
}
#endif

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
static void SessionSDeckCb(SDeckEvent *event, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::HandleSDeckEvent(session, event);
}
#endif

static void FfmpegFrameCb(ChiakiFfmpegDecoder *decoder, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::TriggerFfmpegFrameAvailable(session);
}
