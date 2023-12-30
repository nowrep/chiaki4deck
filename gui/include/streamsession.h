// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_STREAMSESSION_H
#define CHIAKI_STREAMSESSION_H

#include <chiaki/session.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/opusencoder.h>
#include <chiaki/ffmpegdecoder.h>

#if CHIAKI_LIB_ENABLE_PI_DECODER
#include <chiaki/pidecoder.h>
#endif

#if CHIAKI_GUI_ENABLE_SETSU
#include <setsu.h>
#include <chiaki/orientation.h>
#endif

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
#include <sdeck.h>
#endif

#include "exception.h"
#include "sessionlog.h"
#include "controllermanager.h"
#include "settings.h"

#include <QObject>
#include <QImage>
#include <QMouseEvent>
#include <QTimer>
#include <QQueue>
#include <QElapsedTimer>
#if CHIAKI_GUI_ENABLE_SPEEX
#include <QQueue>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#endif

class QKeyEvent;
class Settings;

class ChiakiException: public Exception
{
	public:
		explicit ChiakiException(const QString &msg) : Exception(msg) {};
};

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
typedef struct haptic_packet_t
{
   	int16_t haptic_packet[30];
    uint64_t timestamp;
} haptic_packet_t;
#endif

struct StreamSessionConnectInfo
{
	Settings *settings;
	QMap<Qt::Key, int> key_map;
	Decoder decoder;
	QString hw_decoder;
	Renderer renderer;
	QString audio_out_device;
	QString audio_in_device;
	uint32_t log_level_mask;
	QString log_file;
	ChiakiTarget target;
	QString host;
	QByteArray regist_key;
	QByteArray morning;
	QString initial_login_pin;
	ChiakiConnectVideoProfile video_profile;
	unsigned int audio_buffer_size;
	bool fullscreen;
	bool zoom;
	bool stretch;
	bool enable_keyboard;
	bool enable_dualsense;
	bool buttons_by_pos;
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	bool vertical_sdeck;
# endif
#if CHIAKI_GUI_ENABLE_SPEEX
	bool speech_processing_enabled;
	int32_t noise_suppress_level;
	int32_t echo_suppress_level;
#endif

	StreamSessionConnectInfo(
			Settings *settings,
			ChiakiTarget target,
			QString host,
			QByteArray regist_key,
			QByteArray morning,
			QString initial_login_pin,
			bool fullscreen,
			bool zoom,
			bool stretch);
};

struct MicBuf
{
	int16_t *buf;
	uint32_t size_bytes;
	uint32_t current_byte;
};

class StreamSession : public QObject
{
	friend class StreamSessionPrivate;

	Q_OBJECT
	Q_PROPERTY(QString host READ GetHost CONSTANT)
	Q_PROPERTY(double measuredBitrate READ GetMeasuredBitrate NOTIFY MeasuredBitrateChanged)
	Q_PROPERTY(bool muted READ GetMuted WRITE SetMuted NOTIFY MutedChanged)

	private:
		SessionLog log;
		ChiakiSession session;
		ChiakiOpusDecoder opus_decoder;
		ChiakiOpusEncoder opus_encoder;
		bool connected;
		bool muted;
		bool mic_connected;
		bool allow_unmute;
		bool input_blocked;
		QString host;
		double measured_bitrate = 0;

		QHash<int, Controller *> controllers;
#if CHIAKI_GUI_ENABLE_SETSU
		Setsu *setsu;
		QMap<QPair<QString, SetsuTrackingId>, uint8_t> setsu_ids;
		ChiakiControllerState setsu_state;
		SetsuDevice *setsu_motion_device;
		ChiakiOrientationTracker orient_tracker;
		bool orient_dirty;
#endif

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		SDeck *sdeck;
		ChiakiControllerState sdeck_state;
		int haptics_sdeck;
		QQueue<haptic_packet_t> sdeck_hapticl;
		QQueue<haptic_packet_t> sdeck_hapticr;
		int16_t * sdeck_haptics_senderl;
		int16_t * sdeck_haptics_senderr;
		int sdeck_queue_segment;
		uint64_t sdeck_last_haptic;
		bool sdeck_skipl, sdeck_skipr;
		ChiakiOrientationTracker sdeck_orient_tracker;
		bool sdeck_orient_dirty;
		bool vertical_sdeck;
#endif
		float PS_TOUCHPAD_MAX_X, PS_TOUCHPAD_MAX_Y;
		ChiakiControllerState keyboard_state;
		ChiakiControllerState touch_state;
		QMap<int, uint8_t> touch_tracker;
		int8_t mouse_touch_id;

		ChiakiFfmpegDecoder *ffmpeg_decoder;
		void TriggerFfmpegFrameAvailable();
#if CHIAKI_LIB_ENABLE_PI_DECODER
		ChiakiPiDecoder *pi_decoder;
#endif

		QString audio_out_device_name;
		QString audio_in_device_name;
		SDL_AudioDeviceID audio_out;
		SDL_AudioDeviceID audio_in;
		size_t audio_out_sample_size;
		unsigned int audio_buffer_size;
#if CHIAKI_GUI_ENABLE_SPEEX
		SpeexEchoState *echo_state;
		SpeexPreprocessState *preprocess_state;
		bool speech_processing_enabled;
		uint8_t *echo_resampler_buf, *mic_resampler_buf;
		QQueue<int16_t *> echo_to_cancel;
#endif
		SDL_AudioDeviceID haptics_output;
		uint8_t *haptics_resampler_buf;
		MicBuf mic_buf;
		QMap<Qt::Key, int> key_map;

		void PushAudioFrame(int16_t *buf, size_t samples_count);
		void PushHapticsFrame(uint8_t *buf, size_t buf_size);
#if CHIAKI_GUI_ENABLE_SETSU
		void HandleSetsuEvent(SetsuEvent *event);
#endif
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		void HandleSDeckEvent(SDeckEvent *event);
#endif

	private slots:
		void InitAudio(unsigned int channels, unsigned int rate);
		void InitMic(unsigned int channels, unsigned int rate);
		void InitHaptics();
		void Event(ChiakiEvent *event);
		void DisconnectHaptics();
		void ConnectHaptics();
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		void SdeckQueueHaptics(haptic_packet_t packetl, haptic_packet_t packetr);
		void ConnectSdeckHaptics();
#endif

	public:
		explicit StreamSession(const StreamSessionConnectInfo &connect_info, QObject *parent = nullptr);
		~StreamSession();

		bool IsConnected()	{ return connected; }

		void Start();
		void Stop();
		void GoToBed();
		void ToggleMute();
		void SetLoginPIN(const QString &pin);
		QString GetHost() { return host; }
		double GetMeasuredBitrate()	{ return measured_bitrate; }
		bool GetMuted()	{ return muted; }
		void SetMuted(bool enable)	{ if (enable != muted) ToggleMute(); }

		ChiakiLog *GetChiakiLog()				{ return log.GetChiakiLog(); }
		QList<Controller *> GetControllers()	{ return controllers.values(); }
		ChiakiFfmpegDecoder *GetFfmpegDecoder()	{ return ffmpeg_decoder; }
#if CHIAKI_LIB_ENABLE_PI_DECODER
		ChiakiPiDecoder *GetPiDecoder()	{ return pi_decoder; }
#endif
		void HandleKeyboardEvent(QKeyEvent *event);
		void HandleTouchEvent(QTouchEvent *event);
		void HandleMouseReleaseEvent(QMouseEvent *event);
		void HandleMousePressEvent(QMouseEvent *event);
		void HandleMouseMoveEvent(QMouseEvent *event, float width, float height);
		void ReadMic(const QByteArray &micdata);

		void BlockInput(bool block) { input_blocked = block; }

	signals:
		void FfmpegFrameAvailable();
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		void SdeckHapticPushed(haptic_packet_t packetl, haptic_packet_t packetr);
#endif
		void SessionQuit(ChiakiQuitReason reason, const QString &reason_str);
		void LoginPINRequested(bool incorrect);
		void MeasuredBitrateChanged();
		void MutedChanged();

	private slots:
		void UpdateGamepads();
		void SendFeedbackState();
};

Q_DECLARE_METATYPE(ChiakiQuitReason)
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
Q_DECLARE_METATYPE(haptic_packet_t)
#endif

#endif // CHIAKI_STREAMSESSION_H
