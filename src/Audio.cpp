
/*****************************************************************************************************************************************************
    audio.cpp

    Created on: Oct 28.2018                                                                                                  */char audioI2SVers[] ="\
    Version 3.3.2f                                                                                                                                ";
/*  Updated on: Jun 26.2025

    Author: Wolle (schreibfaul1)
    Audio library for ESP32, ESP32-S3 or ESP32-P4
    Arduino Vers. V3 is mandatory
    PSRAM is mandatory
    external DAC is mandatory

*****************************************************************************************************************************************************/

#include "Audio.h"
#include "aac_decoder/aac_decoder.h"
#include "flac_decoder/flac_decoder.h"
#include "mp3_decoder/mp3_decoder.h"
#include "opus_decoder/opus_decoder.h"
#include "vorbis_decoder/vorbis_decoder.h"
#include "psram_unique_ptr.hpp"

template <typename... Args>
void AUDIO_INFO(const char* fmt, Args&&... args) {
    ps_ptr<char> result;

    // First run: determine size
    int len = std::snprintf(nullptr, 0, fmt, std::forward<Args>(args)...);
    if (len <= 0) return;

    result.alloc(len + 1, "result");
    char* dst = result.get();
    if (!dst) return;  // Or error treatment
    std::snprintf(dst, len + 1, fmt, std::forward<Args>(args)...);
    if(audio_info) audio_info(result.get());
    result.reset();
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
AudioBuffer::AudioBuffer(size_t maxBlockSize) {
    if(maxBlockSize) m_resBuffSizeRAM = maxBlockSize;
    if(maxBlockSize) m_maxBlockSize = maxBlockSize;
}

AudioBuffer::~AudioBuffer() {
    if(m_buffer) free(m_buffer);
    m_buffer = NULL;
}

int32_t AudioBuffer::getBufsize() { return m_buffSize; }

void AudioBuffer::setBufsize(size_t mbs) {
    m_buffSizePSRAM = m_buffSizeRAM = m_buffSize = mbs;
    return;
}

size_t AudioBuffer::init() {
    if(m_buffer) free(m_buffer);
    m_buffer = NULL;
    if(m_buffSizePSRAM > 0) { // PSRAM found, AudioBuffer will be allocated in PSRAM
        m_f_psram = true;
        m_buffSize = m_buffSizePSRAM;
        m_buffer = (uint8_t*)ps_calloc(m_buffSize, sizeof(uint8_t));
        m_buffSize = m_buffSizePSRAM - m_resBuffSizePSRAM;
    }
    if(!m_buffer) return 0;
    m_f_init = true;
    resetBuffer();
    return m_buffSize;
}

void AudioBuffer::changeMaxBlockSize(uint16_t mbs) {
    m_maxBlockSize = mbs;
    return;
}

uint16_t AudioBuffer::getMaxBlockSize() { return m_maxBlockSize; }

size_t AudioBuffer::freeSpace() {
    if(m_readPtr == m_writePtr) {
        if(m_f_isEmpty == true) m_freeSpace = m_buffSize;
        else m_freeSpace = 0;
    }
    if(m_readPtr < m_writePtr) {
        m_freeSpace = (m_endPtr - m_writePtr) + (m_readPtr - m_buffer);
    }
    if(m_readPtr > m_writePtr) {
        m_freeSpace = m_readPtr - m_writePtr;
    }
    return m_freeSpace;
}

size_t AudioBuffer::writeSpace() {
    if(m_readPtr == m_writePtr) {
        if(m_f_isEmpty == true) m_writeSpace = m_endPtr - m_writePtr;
        else m_writeSpace = 0;
    }
    if(m_readPtr < m_writePtr) {
        m_writeSpace = m_endPtr - m_writePtr;
    }
    if(m_readPtr > m_writePtr) {
        m_writeSpace = m_readPtr - m_writePtr;
    }
    return m_writeSpace;
}

size_t AudioBuffer::bufferFilled() {
    if(m_readPtr == m_writePtr) {
        if(m_f_isEmpty == true) m_dataLength = 0;
        else m_dataLength = m_buffSize;
    }
    if(m_readPtr < m_writePtr) {
        m_dataLength = m_writePtr - m_readPtr;
    }
    if(m_readPtr > m_writePtr) {
        m_dataLength = (m_endPtr - m_readPtr) + (m_writePtr - m_buffer);
    }
    return m_dataLength;
}

size_t AudioBuffer::getMaxAvailableBytes() {
    if(m_readPtr == m_writePtr) {
    //   if(m_f_start)m_dataLength = 0;
        if(m_f_isEmpty == true) m_dataLength = 0;
        else m_dataLength = (m_endPtr - m_readPtr);
    }
    if(m_readPtr < m_writePtr) {
        m_dataLength = m_writePtr - m_readPtr;
    }
    if(m_readPtr > m_writePtr) {
        m_dataLength = (m_endPtr - m_readPtr);
    }
    return m_dataLength;
}

void AudioBuffer::bytesWritten(size_t bw) {
    if(!bw) return;
    m_writePtr += bw;
    if(m_writePtr == m_endPtr) { m_writePtr = m_buffer; }
    if(m_writePtr > m_endPtr) log_e("m_writePtr %i, m_endPtr %i", m_writePtr, m_endPtr);
    m_f_isEmpty = false;
}

void AudioBuffer::bytesWasRead(size_t br) {
    if(!br) return;
    m_readPtr += br;
    if(m_readPtr >= m_endPtr) {
        size_t tmp = m_readPtr - m_endPtr;
        m_readPtr = m_buffer + tmp;
    }
    if(m_readPtr == m_writePtr) m_f_isEmpty = true;
}

uint8_t* AudioBuffer::getWritePtr() { return m_writePtr; }

uint8_t* AudioBuffer::getReadPtr() {
    int32_t len = m_endPtr - m_readPtr;
    if(len < m_maxBlockSize) {                            // be sure the last frame is completed
        memcpy(m_endPtr, m_buffer, m_maxBlockSize - (len)); // cpy from m_buffer to m_endPtr with len
    }
    return m_readPtr;
}

void AudioBuffer::resetBuffer() {
    m_writePtr = m_buffer;
    m_readPtr = m_buffer;
    m_endPtr = m_buffer + m_buffSize;
    m_f_isEmpty = true;
}

uint32_t AudioBuffer::getWritePos() { return m_writePtr - m_buffer; }

uint32_t AudioBuffer::getReadPos() { return m_readPtr - m_buffer; }
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// clang-format off
Audio::Audio(uint8_t i2sPort) {

    mutex_playAudioData = xSemaphoreCreateMutex();
    mutex_audioTask     = xSemaphoreCreateMutex();

    if(!psramFound()) log_e("audioI2S requires PSRAM!");

    clientsecure.setInsecure();
    m_i2s_num = i2sPort;  // i2s port number

    // -------- I2S configuration -------------------------------------------------------------------------------------------
    m_i2s_chan_cfg.id            = (i2s_port_t)m_i2s_num;  // I2S_NUM_AUTO, I2S_NUM_0, I2S_NUM_1
    m_i2s_chan_cfg.role          = I2S_ROLE_MASTER;        // I2S controller master role, bclk and lrc signal will be set to output
    m_i2s_chan_cfg.dma_desc_num  = 16;                      // number of DMA buffer
    m_i2s_chan_cfg.dma_frame_num = 128;                   // I2S frame number in one DMA buffer.
    m_i2s_chan_cfg.auto_clear    = true;                   // i2s will always send zero automatically if no data to send
    i2s_new_channel(&m_i2s_chan_cfg, &m_i2s_tx_handle, NULL);

    m_i2s_std_cfg.slot_cfg                = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO); // Set to enable bit shift in Philips mode
    m_i2s_std_cfg.gpio_cfg.bclk           = I2S_GPIO_UNUSED;           // BCLK, Assignment in setPinout()
    m_i2s_std_cfg.gpio_cfg.din            = I2S_GPIO_UNUSED;           // not used
    m_i2s_std_cfg.gpio_cfg.dout           = I2S_GPIO_UNUSED;           // DOUT, Assignment in setPinout()
    m_i2s_std_cfg.gpio_cfg.mclk           = I2S_GPIO_UNUSED;           // MCLK, Assignment in setPinout()
    m_i2s_std_cfg.gpio_cfg.ws             = I2S_GPIO_UNUSED;           // LRC,  Assignment in setPinout()
    m_i2s_std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    m_i2s_std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    m_i2s_std_cfg.gpio_cfg.invert_flags.ws_inv   = false;
    m_i2s_std_cfg.clk_cfg.sample_rate_hz = 48000;
    m_i2s_std_cfg.clk_cfg.clk_src        = I2S_CLK_SRC_DEFAULT;        // Select PLL_F160M as the default source clock
    m_i2s_std_cfg.clk_cfg.mclk_multiple  = I2S_MCLK_MULTIPLE_128;      // mclk = sample_rate * 256
    i2s_channel_init_std_mode(m_i2s_tx_handle, &m_i2s_std_cfg);
    I2Sstart();
    m_sampleRate = m_i2s_std_cfg.clk_cfg.sample_rate_hz;

    for(int i = 0; i < 3; i++) {
        m_filter[i].a0 = 1;
        m_filter[i].a1 = 0;
        m_filter[i].a2 = 0;
        m_filter[i].b1 = 0;
        m_filter[i].b2 = 0;
    }
    computeLimit();  // first init, vol = 21, vol_steps = 21
    startAudioTask();
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
Audio::~Audio() {
    stopSong();
    setDefaults();

    i2s_channel_disable(m_i2s_tx_handle);
    i2s_del_channel(m_i2s_tx_handle);
    stopAudioTask();
    vSemaphoreDelete(mutex_playAudioData);
    vSemaphoreDelete(mutex_audioTask);
}
// clang-format on
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::initInBuff() {
    if(!InBuff.isInitialized()) {
        size_t size = InBuff.init();
        if(size > 0) {
            AUDIO_INFO("PSRAM %sfound, inputBufferSize: %u bytes", InBuff.havePSRAM() ? "" : "not ", size - 1);
        }
    }
    changeMaxBlockSize(1600); // default size mp3 or aac
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
esp_err_t Audio::I2Sstart() {
    zeroI2Sbuff();
    return i2s_channel_enable(m_i2s_tx_handle);
}

esp_err_t Audio::I2Sstop() {
    m_outBuff.clear(); // Clear OutputBuffer
    m_samplesBuff48K.clear(); // Clear samplesBuff48K
    std::fill(std::begin(m_inputHistory), std::end(m_inputHistory), 0); // Clear history in samplesBuff48K
    return i2s_channel_disable(m_i2s_tx_handle);
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::zeroI2Sbuff(){
    uint8_t buff[2] = {0, 0}; // From IDF V5 there is no longer the zero_dma_buff() function.
    size_t bytes_loaded = 0;  // As a replacement, we write a small amount of zeros in the buffer and thus reset the entire buffer.
    i2s_channel_preload_data(m_i2s_tx_handle, buff, 2, &bytes_loaded);
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Audio::setDefaults() {
    stopSong();
    initInBuff(); // initialize InputBuffer if not already done
    InBuff.resetBuffer();
    MP3Decoder_FreeBuffers();
    FLACDecoder_FreeBuffers();
    AACDecoder_FreeBuffers();
    OPUSDecoder_FreeBuffers();
    VORBISDecoder_FreeBuffers();
    m_outBuff.clear(); // Clear OutputBuffer
    m_samplesBuff48K.clear(); // Clear samplesBuff48K
    vector_clear_and_shrink(m_playlistURL);
    vector_clear_and_shrink(m_playlistContent);
    m_hashQueue.clear();
    m_hashQueue.shrink_to_fit(); // uint32_t vector
    client.stop();
    clientsecure.stop();
    _client = static_cast<WiFiClient*>(&client); /* default to *something* so that no NULL deref can happen */
    ts_parsePacket(0, 0, 0);                     // reset ts routine
    m_lastM3U8host.reset();

    AUDIO_INFO("buffers freed, free Heap: %lu bytes", (long unsigned int)ESP.getFreeHeap());

    m_f_timeout = false;
    m_f_chunked = false; // Assume not chunked
    m_f_firstmetabyte = false;
    m_f_playing = false;
//    m_f_ssl = false;
    m_f_metadata = false;
    m_f_tts = false;
    m_f_firstCall = true;        // InitSequence for processWebstream and processLocalFile
    m_f_firstCurTimeCall = true; // InitSequence for computeAudioTime
    m_f_firstM3U8call = true;    // InitSequence for parsePlaylist_M3U8
    m_f_firstPlayCall = true;    // InitSequence for playAudioData
//    m_f_running = false;       // already done in stopSong
    m_f_unsync = false;   // set within ID3 tag but not used
    m_f_exthdr = false;   // ID3 extended header
    m_f_rtsp = false;     // RTSP (m3u8)stream
    m_f_m3u8data = false; // set again in processM3U8entries() if necessary
    m_f_continue = false;
    m_f_ts = false;
    m_f_ogg = false;
    m_f_m4aID3dataAreRead = false;
    m_f_stream = false;
    m_f_decode_ready = false;
    m_f_eof = false;
    m_f_ID3v1TagFound = false;
    m_f_lockInBuffer = false;
    m_f_acceptRanges = false;

    m_streamType = ST_NONE;
    m_codec = CODEC_NONE;
    m_playlistFormat = FORMAT_NONE;
    m_dataMode = AUDIO_NONE;
    m_streamTitle.assign("");
    m_resumeFilePos = -1;
    m_audioCurrentTime = 0; // Reset playtimer
    m_audioFileDuration = 0;
    m_audioDataStart = 0;
    m_audioDataSize = 0;
    m_avr_bitrate = 0;     // the same as m_bitrate if CBR, median if VBR
    m_bitRate = 0;         // Bitrate still unknown
    m_bytesNotDecoded = 0; // counts all not decodable bytes
    m_chunkcount = 0;      // for chunked streams
    m_contentlength = 0;   // If Content-Length is known, count it
    m_curSample = 0;
    m_metaint = 0;        // No metaint yet
    m_LFcount = 0;        // For end of header detection
    m_controlCounter = 0; // Status within readID3data() and readWaveHeader()
    m_channels = 2;       // assume stereo #209
    m_fileSize = 0;
    m_ID3Size = 0;
    m_haveNewFilePos = 0;
    m_validSamples = 0;
    m_M4A_chConfig = 0;
    m_M4A_objectType = 0;
    m_M4A_sampleRate = 0;
    m_sumBytesDecoded = 0;
    m_opus_mode = 0;
    m_vuLeft = m_vuRight = 0; // #835
    std::fill(std::begin(m_inputHistory), std::end(m_inputHistory), 0);
    if(m_f_reset_m3u8Codec){m_m3u8Codec = CODEC_AAC;} // reset to default
    m_f_reset_m3u8Codec = true;
    m_resampleCursor = 0.0f;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::setConnectionTimeout(uint16_t timeout_ms, uint16_t timeout_ms_ssl) {
    if(timeout_ms) m_timeout_ms = timeout_ms;
    if(timeout_ms_ssl) m_timeout_ms_ssl = timeout_ms_ssl;
}

/*
    Text to speech API provides a speech endpoint based on our TTS (text-to-speech) model.
    More info: https://platform.openai.com/docs/guides/text-to-speech/text-to-speech

    Request body:
    model (string) [Required] - One of the available TTS models: tts-1 or tts-1-hd
    input (string) [Required] - The text to generate audio for. The maximum length is 4096 characters.
    instructions (string) [Optional] - A description of the desired characteristics of the generated audio.
    voice (string) [Required] - The voice to use when generating the audio. Supported voices are alloy, echo, fable, onyx, nova, and shimmer.
    response_format (string) [Optional] - Defaults to mp3. The format to audio in. Supported formats are mp3, opus, aac, and flac.
    speed (number) [Optional] - Defaults to 1. The speed of the generated audio. Select a value from 0.25 to 4.0. 1.0 is the default.

    Usage: audio.openai_speech(OPENAI_API_KEY, "tts-1", input, instructions, "shimmer", "mp3", "1");
*/
bool Audio::openai_speech(const String& api_key, const String& model, const String& input, const String& instructions, const String& voice, const String& response_format, const String& speed) {
    ps_ptr<char> host;
    host.assign("api.openai.com");
    char path[] = "/v1/audio/speech";

    if (input == "") {
        AUDIO_INFO("input text is empty");
        stopSong();
        return false;
    }
    xSemaphoreTakeRecursive(mutex_playAudioData, 0.3 * configTICK_RATE_HZ);

    setDefaults();
    m_f_ssl = true;

    m_speechtxt.assign(input.c_str());

    // Escape special characters in input
    String input_clean = "";
    for (int i = 0; i < input.length(); i++) {
        char c = input.charAt(i);
        if (c == '\"') {
            input_clean += "\\\"";
        } else if (c == '\n') {
            input_clean += "\\n";
        } else if (c == '\r') {
            input_clean += "\\r";
        } else if (c == '\t') {
            input_clean += "\\t";
        } else if (c == '\\') {
            input_clean += "\\\\";
        } else if (c == '\b') {
            input_clean += "\\b";
        } else if (c == '\f') {
            input_clean += "\\f";
        } else {
            input_clean += c;
        }
    }

    // Escape special characters in instructions
    String instructions_clean = "";
    for (int i = 0; i < instructions.length(); i++) {
        char c = instructions.charAt(i);
        if (c == '\"') {
            instructions_clean += "\\\"";
        } else if (c == '\n') {
            instructions_clean += "\\n";
        } else if (c == '\r') {
            instructions_clean += "\\r";
        } else if (c == '\t') {
            instructions_clean += "\\t";
        } else if (c == '\\') {
            instructions_clean += "\\\\";
        } else if (c == '\b') {
            instructions_clean += "\\b";
        } else if (c == '\f') {
            instructions_clean += "\\f";
        } else {
            instructions_clean += c;
        }
    }

    String post_body = "{"
        "\"model\": \"" + model + "\"," +
        "\"input\": \"" + input_clean + "\"," +
        "\"instructions\": \"" + instructions_clean + "\"," +
        "\"voice\": \"" + voice + "\"," +
        "\"response_format\": \"" + response_format + "\"," +
        "\"speed\": " + speed +
    "}";

    String http_request =
        "POST " + String(path) + " HTTP/1.0\r\n" // UNKNOWN ERROR CODE (0050) - crashing on HTTP/1.1 need to use HTTP/1.0
        + "Host: " + host.get() + "\r\n"
        + "Authorization: Bearer " + api_key + "\r\n"
        + "Accept-Encoding: identity;q=1,*;q=0\r\n"
        + "User-Agent: nArija/1.0\r\n"
        + "Content-Type: application/json; charset=utf-8\r\n"
        + "Content-Length: " + post_body.length() + "\r\n"
        + "Connection: close\r\n" + "\r\n"
        + post_body + "\r\n"
    ;

    bool res = true;
    int port = 443;
    _client = static_cast<WiFiClient*>(&clientsecure);

    uint32_t t = millis();
    AUDIO_INFO("Connect to: \"%s\"", host.get());
    res = _client->connect(host.get(), port, m_timeout_ms_ssl);
    if (res) {
        uint32_t dt = millis() - t;
        m_lastHost.assign(host.get());
        AUDIO_INFO("%s has been established in %lu ms", m_f_ssl ? "SSL" : "Connection", (long unsigned int)dt);
        m_f_running = true;
    }

    m_expectedCodec = CODEC_NONE;
    m_expectedPlsFmt = FORMAT_NONE;

    if (res) {
        _client->print(http_request);
        if (response_format == "mp3") m_expectedCodec  = CODEC_MP3;
        if (response_format == "opus") m_expectedCodec  = CODEC_OPUS;
        if (response_format == "aac") m_expectedCodec  = CODEC_AAC;
        if (response_format == "flac") m_expectedCodec  = CODEC_FLAC;
        m_dataMode = HTTP_RESPONSE_HEADER;
        m_f_tts = true;
    } else {
        AUDIO_INFO("Request %s failed!", host.get());
    }
    xSemaphoreGiveRecursive(mutex_playAudioData);
    return res;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
ps_struct_ptr<hwoe_t>Audio::dismantle_host(const char* host){
    if (!host) return {};

    ps_struct_ptr<hwoe_t> result;
    result.alloc();

    const char* p = host;

    // 🔐 1. SSL check
    if (strncmp(p, "https://", 8) == 0) {
        result->ssl = true;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        result->ssl = false;
        p += 7;
    } else {  // No valid scheme -> error or acceptance http
        result->ssl = false;
    }

    // ❓ 2. extract host (from p to ':' or '/' or '?')
    const char* host_start = p;
    const char* port_sep = strchr(p, ':');
    const char* path_sep = strchr(p, '/');
    const char* query_sep = strchr(p, '?');

    const char* host_end = p + strlen(p);  // default: end of string
    if (port_sep  && port_sep  < host_end) host_end = port_sep;
    if (path_sep  && path_sep  < host_end) host_end = path_sep;
    if (query_sep && query_sep < host_end) host_end = query_sep;
    result->hwoe.copy_from(host_start, host_end - host_start);

    // ❓ 3. extract port
    result->port = result->ssl ? 443 : 80; // default
    if (port_sep && (!path_sep || port_sep < path_sep)) {
        result->port = atoi(port_sep + 1);
    }

    // ❓ 4. extract extension (path)
    if (path_sep) {
        const char* path_start = path_sep + 1;
        const char* path_end = query_sep ? query_sep : host + strlen(host);
        result->extension.copy_from(path_start, path_end - path_start);
    }

    // ❓ 5. extract query string
    if (query_sep) {
        result->query_string.assign(query_sep + 1);
    }

    return result;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::connecttohost(const char* host, const char* user, const char* pwd) { // user and pwd for authentification only, can be empty

    if(!host) { AUDIO_INFO("Hostaddress is empty");     stopSong(); return false;}
    if (strlen(host) > 2048) { AUDIO_INFO("Hostaddress is too long");  stopSong(); return false;} // max length in Chrome DevTools

    bool     res           = false; // return value
    uint16_t port          = 0;     // port number
    uint16_t authLen       = 0;     // length of authorization
    uint32_t timestamp     = 0;     // timeout surveillance

    ps_ptr<char> c_host;       // copy of host
    ps_ptr<char> hwoe;         // host without extension
    ps_ptr<char> extension;    // extension
    ps_ptr<char> query_string; // parameter
    ps_ptr<char> path;         // extension + '?' + parameter
    ps_ptr<char> rqh;          // request header

    xSemaphoreTakeRecursive(mutex_playAudioData, 0.3 * configTICK_RATE_HZ);

    c_host.copy_from(host);
    c_host.trim();
    auto dismantledHost = dismantle_host(c_host.get());

//  https://edge.live.mp3.mdn.newmedia.nacamar.net:8000/ps-charivariwb/livestream.mp3;?user=ps-charivariwb;&pwd=ps-charivariwb-------
//      |   |                                     |    |                              |
//      |   |                                     |    |                              |             (query string)
//  ssl?|   |<-----host without extension-------->|port|<----- --extension----------->|<-first parameter->|<-second parameter->.......
//                                                     |
//                                                     |<-----------------------------path------------------------------------>......

    m_f_ssl = dismantledHost->ssl;
    port = dismantledHost->port;
    if(dismantledHost->hwoe.valid()) hwoe.clone_from(dismantledHost->hwoe);
    if(dismantledHost->extension.valid()) extension.clone_from(dismantledHost->extension);
    if(dismantledHost->query_string.valid()) query_string.clone_from(dismantledHost->query_string);

    if(extension.valid()) path.assign(extension.get());
    if(query_string.valid()){path.append("?"); path.append(query_string.get());}
    if(!hwoe.valid()) hwoe.assign("");
    if(!extension.valid()) extension.assign("");
    if(!path.valid()) path.assign("");

    path = urlencode(path.get(), true);

    // optional basic authorization
    if(user && pwd) authLen = strlen(user) + strlen(pwd);
    ps_ptr <char>authorization;
    ps_ptr<char> toEncode;
    authorization.alloc(base64_encode_expected_len(authLen + 1) + 1, "authorization");
    authorization.clear();
    if(authLen > 0) {
        toEncode.assign(user);
        toEncode.append(":");
        toEncode.append(pwd);
        b64encode((const char*)toEncode.get(), toEncode.strlen(), authorization.get());
    }

    setDefaults();
                       rqh.assign("GET /");
                       rqh.append(path.get());
                       rqh.append(" HTTP/1.1\r\n");
                       rqh.append("Host: ");
                       rqh.append(hwoe.get());
                       rqh.append("\r\n");
                       rqh.append("Icy-MetaData:1\r\n");
                       rqh.append("Icy-MetaData:2\r\n");
                       rqh.append("Accept:*/*\r\n");
                       rqh.append("User-Agent: VLC/3.0.21 LibVLC/3.0.21 AppleWebKit/537.36 (KHTML, like Gecko)\r\n");
    if(authLen > 0) {  rqh.append("Authorization: Basic ");
                       rqh.append(authorization.get());
                       rqh.append("\r\n"); }
                       rqh.append("Accept-Encoding: identity;q=1,*;q=0\r\n");
                       rqh.append("Connection: keep-alive\r\n\r\n");

    if(m_f_ssl) { _client = static_cast<WiFiClient*>(&clientsecure);}
    else        { _client = static_cast<WiFiClient*>(&client); }

    timestamp = millis();
    _client->setTimeout(m_f_ssl ? m_timeout_ms_ssl : m_timeout_ms);

    AUDIO_INFO("connect to: \"%s\" on port %d path \"/%s\"", hwoe.get(), port, path.get());
    res = _client->connect(hwoe.get(), port);

    m_expectedCodec = CODEC_NONE;
    m_expectedPlsFmt = FORMAT_NONE;

    if(res) {
        uint32_t dt = millis() - timestamp;
        m_lastHost.assign(c_host.get());
        AUDIO_INFO("%s has been established in %lu ms", m_f_ssl ? "SSL" : "Connection", (long unsigned int)dt);
        m_f_running = true;
        _client->print(rqh.get());
        if(extension.ends_with_icase( ".mp3" ))      m_expectedCodec  = CODEC_MP3;
        if(extension.ends_with_icase( ".aac" ))      m_expectedCodec  = CODEC_AAC;
        if(extension.ends_with_icase( ".wav" ))      m_expectedCodec  = CODEC_WAV;
        if(extension.ends_with_icase( ".m4a" ))      m_expectedCodec  = CODEC_M4A;
        if(extension.ends_with_icase( ".ogg" ))      m_expectedCodec  = CODEC_OGG;
        if(extension.ends_with_icase( ".flac"))      m_expectedCodec  = CODEC_FLAC;
        if(extension.ends_with_icase( "-flac"))      m_expectedCodec  = CODEC_FLAC;
        if(extension.ends_with_icase( ".opus"))      m_expectedCodec  = CODEC_OPUS;
        if(extension.ends_with_icase( "/opus"))      m_expectedCodec  = CODEC_OPUS;
        if(extension.ends_with_icase( ".asx" ))      m_expectedPlsFmt = FORMAT_ASX;
        if(extension.ends_with_icase( ".m3u" ))      m_expectedPlsFmt = FORMAT_M3U;
        if(extension.ends_with_icase( ".pls" ))      m_expectedPlsFmt = FORMAT_PLS;
        if(extension.contains(".m3u8"))             {m_expectedPlsFmt = FORMAT_M3U8; if(audio_lasthost) audio_lasthost(m_lastHost.get());}

        m_dataMode = HTTP_RESPONSE_HEADER; // Handle header
        m_streamType = ST_WEBSTREAM;
    }
    else {
        AUDIO_INFO("Request %s failed!", c_host.get());
        m_f_running = false;
        if(audio_showstation) audio_showstation("");
        if(audio_showstreamtitle) audio_showstreamtitle("");
        if(audio_icydescription) audio_icydescription("");
        if(audio_icyurl) audio_icyurl("");
    }
    xSemaphoreGiveRecursive(mutex_playAudioData);
    return res;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::httpPrint(const char* host) {
    // user and pwd for authentification only, can be empty
    if(!m_f_running) return false;
    if(host == NULL) {AUDIO_INFO("Hostaddress is empty"); stopSong(); return false;}

    uint16_t port = 0;         // port number
    ps_ptr<char> c_host;       // copy of host
    ps_ptr<char> hwoe;         // host without extension
    ps_ptr<char> extension;    // extension
    ps_ptr<char> query_string; // parameter
     ps_ptr<char> path;        // extension + '?' + parameter
    ps_ptr<char> rqh;          // request header
    ps_ptr<char> last_hwoe;    // m_lastM3U8host without extension

    c_host.copy_from(host);
    c_host.trim();
    auto dismantledHost = dismantle_host(c_host.get());

//  https://edge.live.mp3.mdn.newmedia.nacamar.net:8000/ps-charivariwb/livestream.mp3;?user=ps-charivariwb;&pwd=ps-charivariwb-------
//      |   |                                     |    |                              |
//      |   |                                     |    |                              |             (query string)
//  ssl?|   |<-----host without extension-------->|port|<----- --extension----------->|<-first parameter->|<-second parameter->.......
//                                                     |
//                                                     |<-----------------------------path------------------------------------------->

    m_f_ssl = dismantledHost->ssl;
    port = dismantledHost->port;
    if(dismantledHost->hwoe.valid()) hwoe.clone_from(dismantledHost->hwoe);
    if(dismantledHost->extension.valid()) extension.clone_from(dismantledHost->extension);
    if(dismantledHost->query_string.valid()) query_string.clone_from(dismantledHost->query_string);

    if(extension.valid()) path.assign(extension.get());
    if(query_string.valid()){path.append("?"); path.append(query_string.get());}
    if(!hwoe.valid()) hwoe.assign("");
    if(!extension.valid()) extension.assign("");
    if(!path.valid()) path.assign("");

    path = urlencode(path.get(), true);

    // if(!m_lastM3U8host.valid()) m_lastM3U8host.assign("");
    // auto dismantledLastHost = dismantle_host(m_lastM3U8host.get());
    // last_hwoe.clone_from(dismantledLastHost->hwoe);

    rqh.assign("GET /");
    rqh.append(path.get());
    rqh.append(" HTTP/1.1\r\n");
    rqh.append("Host: ");
    rqh.append(hwoe.get());
    rqh.append("\r\n");
    rqh.append("Accept: */*\r\n");
    rqh.append("User-Agent: VLC/3.0.21 LibVLC/3.0.21 AppleWebKit/537.36 (KHTML, like Gecko)\r\n");
    rqh.append("Accept-Encoding: identity;q=1,*;q=0\r\n");
    rqh.append("Connection: keep-alive\r\n\r\n");

    AUDIO_INFO("next URL: \"%s\"", c_host.get());

    if(!_client->connected()) {
         if(m_f_ssl) { _client = static_cast<WiFiClient*>(&clientsecure); if(m_f_ssl && port == 80) port = 443;}
         else        { _client = static_cast<WiFiClient*>(&client); }
        AUDIO_INFO("The host has disconnected, reconnecting");
        if(!_client->connect(hwoe.get(), port)) {
            log_e("connection lost");
            stopSong();
            return false;
        }
    }

    _client->print(rqh.get());

    if(     extension.ends_with_icase(".mp3"))       m_expectedCodec  = CODEC_MP3;
    else if(extension.ends_with_icase(".aac"))       m_expectedCodec  = CODEC_AAC;
    else if(extension.ends_with_icase(".wav"))       m_expectedCodec  = CODEC_WAV;
    else if(extension.ends_with_icase(".m4a"))       m_expectedCodec  = CODEC_M4A;
    else if(extension.ends_with_icase(".flac"))      m_expectedCodec  = CODEC_FLAC;
    else                                             m_expectedCodec  = CODEC_NONE;

    if(     extension.ends_with_icase(".asx"))       m_expectedPlsFmt = FORMAT_ASX;
    else if(extension.ends_with_icase(".m3u"))       m_expectedPlsFmt = FORMAT_M3U;
    else if(extension.contains(".m3u8"))             m_expectedPlsFmt = FORMAT_M3U8;
    else if(extension.ends_with_icase(".pls"))       m_expectedPlsFmt = FORMAT_PLS;
    else                                             m_expectedPlsFmt = FORMAT_NONE;

    m_dataMode = HTTP_RESPONSE_HEADER; // Handle header
    m_streamType = ST_WEBSTREAM;
    m_contentlength = 0;
    m_f_chunked = false;

    return true;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::httpRange(const char* host, uint32_t range){

    if(!m_f_running) return false;
    if(host == NULL) {
        AUDIO_INFO("Hostaddress is empty");
        stopSong();
        return false;
    }
    ps_ptr<char> h_host; // copy of host without http:// or https://

    if(startsWith(host, "https")) m_f_ssl = true;
    else m_f_ssl = false;

    if(m_f_ssl) h_host.append(host + 8);
    else        h_host.append(host + 7);

    int16_t  pos_slash;     // position of "/" in hostname
    int16_t  pos_colon;     // position of ":" in hostname
    int16_t  pos_ampersand; // position of "&" in hostname
    uint16_t port = 80;     // port number

    // In the URL there may be an extension, like noisefm.ru:8000/play.m3u&t=.m3u
    pos_slash = h_host.index_of( "/", 0);
    pos_colon = h_host.index_of( ":", 0);
    if(isalpha(h_host[pos_colon + 1])) pos_colon = -1; // no portnumber follows
    pos_ampersand = h_host.index_of( "&", 0);

    ps_ptr<char> hostwoext; // "skonto.ls.lv:8002" in "skonto.ls.lv:8002/mp3"
    ps_ptr<char> extension; // "/mp3" in "skonto.ls.lv:8002/mp3"

    if(pos_slash > 1) {
        hostwoext.alloc(pos_slash + 1, "hostwoext");
        memcpy(hostwoext.get(), h_host.get(), pos_slash);
        hostwoext[pos_slash] = '\0';
        extension = urlencode(h_host.get() + pos_slash, true);
    }
    else { // url has no extension
        hostwoext.append(h_host.get());
        extension.append("/");
    }

    if((pos_colon >= 0) && ((pos_ampersand == -1) || (pos_ampersand > pos_colon))) {
        port = atoi(h_host.get() + pos_colon + 1); // Get portnumber as integer
        hostwoext[pos_colon] = '\0';         // Host without portnumber
    }

    char rqh[strlen(h_host.get()) + strlen(host) + 300]; // http request header
    rqh[0] = '\0';
    char ch_range[12];
    ltoa(range, ch_range, 10);
    AUDIO_INFO("skip to position: %li", (long int)range);
    strcat(rqh, "GET ");
    strcat(rqh, extension.get());
    strcat(rqh, " HTTP/1.1\r\n");
    strcat(rqh, "Host: ");
    strcat(rqh, hostwoext.get());
    strcat(rqh, "\r\n");
    strcat(rqh, "Range: bytes=");
    strcat(rqh, (const char*)ch_range);
    strcat(rqh, "-\r\n");
    strcat(rqh, "Referer: ");
    strcat(rqh, host);
    strcat(rqh, "\r\n");
    strcat(rqh, "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0.0.0 Safari/537.36\r\n");
    strcat(rqh, "Connection: keep-alive\r\n\r\n");

log_e("%s", rqh);

    _client->stop();
    if(m_f_ssl) { _client = static_cast<WiFiClient*>(&clientsecure); if(m_f_ssl && port == 80) port = 443;}
    else        { _client = static_cast<WiFiClient*>(&client); }
    AUDIO_INFO("The host has disconnected, reconnecting");
    if(!_client->connect(hostwoext.get(), port)) {
        log_e("connection lost");
        stopSong();
        return false;
    }
    _client->print(rqh);
    if(extension.ends_with_icase(".mp3"))       m_expectedCodec  = CODEC_MP3;
    if(extension.ends_with_icase(".aac"))       m_expectedCodec  = CODEC_AAC;
    if(extension.ends_with_icase(".wav"))       m_expectedCodec  = CODEC_WAV;
    if(extension.ends_with_icase(".m4a"))       m_expectedCodec  = CODEC_M4A;
    if(extension.ends_with_icase(".flac"))      m_expectedCodec  = CODEC_FLAC;
    if(extension.ends_with_icase(".asx"))       m_expectedPlsFmt = FORMAT_ASX;
    if(extension.ends_with_icase(".m3u"))       m_expectedPlsFmt = FORMAT_M3U;
    if(extension.contains(".m3u8"))             m_expectedPlsFmt = FORMAT_M3U8;
    if(extension.ends_with_icase(".pls"))       m_expectedPlsFmt = FORMAT_PLS;

    m_dataMode = HTTP_RESPONSE_HEADER; // Handle header
    m_streamType = ST_WEBFILE;
    m_contentlength = 0;
    m_f_chunked = false;

    return true;
}
#ifndef AUDIO_NO_SD_FS
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::connecttoFS(fs::FS& fs, const char* path, int32_t fileStartPos) {

    xSemaphoreTakeRecursive(mutex_playAudioData, 0.3 * configTICK_RATE_HZ);
    ps_ptr<char>c_path;
    ps_ptr<char> audioPath;
    bool res = false;
    m_fileStartPos = fileStartPos;
    uint8_t codec = CODEC_NONE;

    if(!path) {AUDIO_INFO("file path is not set"); goto exit;}  // guard
    c_path.copy_from(path); // copy from path
    c_path.trim();
    if(!c_path.contains(".")) {AUDIO_INFO("No file extension found"); goto exit;}  // guard
    setDefaults(); // free buffers an set defaults

    if(c_path.ends_with_icase(".mp3"))   codec = CODEC_MP3;
    if(c_path.ends_with_icase(".m4a"))   codec = CODEC_M4A;
    if(c_path.ends_with_icase(".aac"))   codec = CODEC_AAC;
    if(c_path.ends_with_icase(".wav"))   codec = CODEC_WAV;
    if(c_path.ends_with_icase(".flac"))  codec = CODEC_FLAC;
    if(c_path.ends_with_icase(".opus")) {codec = CODEC_OPUS; m_f_ogg = true;}
    if(c_path.ends_with_icase(".ogg"))  {codec = CODEC_OGG;  m_f_ogg = true;}
    if(c_path.ends_with_icase(".oga"))  {codec = CODEC_OGG;  m_f_ogg = true;}
    if(codec == CODEC_NONE) {   // guard
        int dotPos = c_path.last_index_of('.');
        AUDIO_INFO("The %s format is not supported", path + dotPos); goto exit;
    }

    if(!c_path.starts_with("/")) c_path.insert("/", 0);

    if(!fs.exists(c_path.get())) {AUDIO_INFO("file not found: %s", c_path.get()); goto exit;}
    AUDIO_INFO("Reading file: \"%s\"", c_path.get());
    m_audiofile = fs.open(c_path.get());
    m_dataMode = AUDIO_LOCALFILE;
    m_fileSize = m_audiofile.size();

    res = initializeDecoder(codec);
    m_codec = codec;
    if(res) m_f_running = true;
    else m_audiofile.close();

exit:
    xSemaphoreGiveRecursive(mutex_playAudioData);
    return res;
}
#endif  // AUDIO_NO_SD_FS
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::connecttospeech(const char* speech, const char* lang) {
    xSemaphoreTakeRecursive(mutex_playAudioData, 0.3 * configTICK_RATE_HZ);

    setDefaults();
    char host[] = "translate.google.com.vn";
    char path[] = "/translate_tts";

    m_speechtxt.assign(speech); // unique pointer takes care of the memory management
    auto urlStr = urlencode(speech, false); // percent encoding
    if(!urlStr.valid()) {
        log_e("out of memory");
        xSemaphoreGiveRecursive(mutex_playAudioData);
        return false;
    }

    ps_ptr<char> req; // request header
    req.assign("GET ");
    req.append(path);
    req.append("?ie=UTF-8&tl=");
    req.append(lang);
    req.append("&client=tw-ob&q=");
    req.append(urlStr.get());
    req.append(" HTTP/1.1\r\n");
    req.append("Host: ");
    req.append(host);
    req.append("\r\n");
    req.append("User-Agent: Mozilla/5.0 \r\n");
    req.append("Accept-Encoding: identity\r\n");
    req.append("Accept: text/html\r\n");
    req.append("Connection: close\r\n\r\n");

    _client = static_cast<WiFiClient*>(&client);
    AUDIO_INFO("connect to \"%s\"", host);
    if(!_client->connect(host, 80)) {
        log_e("Connection failed");
        xSemaphoreGiveRecursive(mutex_playAudioData);
        return false;
    }
    _client->print(req.get());

    m_f_running = true;
    m_f_ssl = false;
    m_f_tts = true;
    m_dataMode = HTTP_RESPONSE_HEADER;
    m_lastHost.assign(host);
    xSemaphoreGiveRecursive(mutex_playAudioData);
    return true;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::showID3Tag(const char* tag, const char* value) {
    ps_ptr<char>id3tag = {};
    // V2.2
    if(!strcmp(tag, "CNT")) id3tag.appendf("Play counter: %s", value, "id3tag");
    if(!strcmp(tag, "COM")) id3tag.appendf("Comments: %s", value, "id3tag");
    if(!strcmp(tag, "CRA")) id3tag.appendf("Audio encryption: %s", value, "id3tag");
    if(!strcmp(tag, "CRM")) id3tag.appendf("Encrypted meta frame: %s", value, "id3tag");
    if(!strcmp(tag, "ETC")) id3tag.appendf("Event timing codes: %s", value, "id3tag");
    if(!strcmp(tag, "EQU")) id3tag.appendf("Equalization: %s", value, "id3tag");
    if(!strcmp(tag, "IPL")) id3tag.appendf("Involved people list: %s", value, "id3tag");
    if(!strcmp(tag, "PIC")) id3tag.appendf("Attached picture: %s", value, "id3tag");
    if(!strcmp(tag, "SLT")) id3tag.appendf("Synchronized lyric/text: %s", value, "id3tag");
    if(!strcmp(tag, "TAL")) id3tag.appendf("Album/Movie/Show title: %s", value, "id3tag");
    if(!strcmp(tag, "TBP")) id3tag.appendf("BPM (Beats Per Minute): %s", value, "id3tag");
    if(!strcmp(tag, "TCM")) id3tag.appendf("Composer: %s", value, "id3tag");
    if(!strcmp(tag, "TCO")) id3tag.appendf("Content type: %s", value, "id3tag");
    if(!strcmp(tag, "TCR")) id3tag.appendf("Copyright message: %s", value, "id3tag");
    if(!strcmp(tag, "TDA")) id3tag.appendf("Date: %s", value, "id3tag");
    if(!strcmp(tag, "TDY")) id3tag.appendf("Playlist delay: %s", value, "id3tag");
    if(!strcmp(tag, "TEN")) id3tag.appendf("Encoded by: %s", value, "id3tag");
    if(!strcmp(tag, "TFT")) id3tag.appendf("File type: %s", value, "id3tag");
    if(!strcmp(tag, "TIM")) id3tag.appendf("Time: %s", value, "id3tag");
    if(!strcmp(tag, "TKE")) id3tag.appendf("Initial key: %s", value, "id3tag");
    if(!strcmp(tag, "TLA")) id3tag.appendf("Language(s): %s", value, "id3tag");
    if(!strcmp(tag, "TLE")) id3tag.appendf("Length: %s", value, "id3tag");
    if(!strcmp(tag, "TMT")) id3tag.appendf("Media type: %s", value, "id3tag");
    if(!strcmp(tag, "TOA")) id3tag.appendf("Original artist(s)/performer(s): %s", value, "id3tag");
    if(!strcmp(tag, "TOF")) id3tag.appendf("Original filename: %s", value, "id3tag");
    if(!strcmp(tag, "TOL")) id3tag.appendf("Original Lyricist(s)/text writer(s): %s", value, "id3tag");
    if(!strcmp(tag, "TOR")) id3tag.appendf("Original release year: %s", value, "id3tag");
    if(!strcmp(tag, "TOT")) id3tag.appendf("Original album/Movie/Show title: %s", value, "id3tag");
    if(!strcmp(tag, "TP1")) id3tag.appendf("Lead artist(s)/Lead performer(s)/Soloist(s)/Performing group: %s", value, "id3tag");
    if(!strcmp(tag, "TP2")) id3tag.appendf("Band/Orchestra/Accompaniment: %s", value, "id3tag");
    if(!strcmp(tag, "TP3")) id3tag.appendf("Conductor/Performer refinement: %s", value, "id3tag");
    if(!strcmp(tag, "TP4")) id3tag.appendf("Interpreted, remixed, or otherwise modified by: %s", value, "id3tag");
    if(!strcmp(tag, "TPA")) id3tag.appendf("Part of a set: %s", value, "id3tag");
    if(!strcmp(tag, "TPB")) id3tag.appendf("Publisher: %s", value, "id3tag");
    if(!strcmp(tag, "TRC")) id3tag.appendf("ISRC (International Standard Recording Code): %s", value, "id3tag");
    if(!strcmp(tag, "TRD")) id3tag.appendf("Recording dates: %s", value, "id3tag");
    if(!strcmp(tag, "TRK")) id3tag.appendf("Track number/Position in set: %s", value, "id3tag");
    if(!strcmp(tag, "TSI")) id3tag.appendf("Size: %s", value, "id3tag");
    if(!strcmp(tag, "TSS")) id3tag.appendf("Software/hardware and settings used for encoding: %s", value, "id3tag");
    if(!strcmp(tag, "TT1")) id3tag.appendf("Content group description: %s", value, "id3tag");
    if(!strcmp(tag, "TT2")) id3tag.appendf("Title/Songname/Content description: %s", value, "id3tag");
    if(!strcmp(tag, "TT3")) id3tag.appendf("Subtitle/Description refinement: %s", value, "id3tag");
    if(!strcmp(tag, "TXT")) id3tag.appendf("Lyricist/text writer: %s", value, "id3tag");
    if(!strcmp(tag, "TXX")) id3tag.appendf("User defined text information frame: %s", value, "id3tag");
    if(!strcmp(tag, "TYE")) id3tag.appendf("Year: %s", value, "id3tag");
    if(!strcmp(tag, "UFI")) id3tag.appendf("Unique file identifier: %s", value, "id3tag");
    if(!strcmp(tag, "ULT")) id3tag.appendf("Unsychronized lyric/text transcription: %s", value, "id3tag");
    if(!strcmp(tag, "WAF")) id3tag.appendf("Official audio file webpage: %s", value, "id3tag");
    if(!strcmp(tag, "WAR")) id3tag.appendf("Official artist/performer webpage: %s", value, "id3tag");
    if(!strcmp(tag, "WAS")) id3tag.appendf("Official audio source webpage: %s", value, "id3tag");
    if(!strcmp(tag, "WCM")) id3tag.appendf("Commercial information: %s", value, "id3tag");
    if(!strcmp(tag, "WCP")) id3tag.appendf("Copyright/Legal information: %s", value, "id3tag");
    if(!strcmp(tag, "WPB")) id3tag.appendf("Publishers official webpage: %s", value, "id3tag");
    if(!strcmp(tag, "WXX")) id3tag.appendf("User defined URL link frame: %s", value, "id3tag");

    // V2.3 V2.4 tags
    if(!strcmp(tag, "COMM")) id3tag.appendf("Comment: %s", value, "id3tag");
    if(!strcmp(tag, "OWNE")) id3tag.appendf("Ownership: %s", value, "id3tag");
    if(!strcmp(tag, "PRIV")) id3tag.appendf("Private: %s", value, "id3tag");
    if(!strcmp(tag, "SYLT")) id3tag.appendf("SynLyrics: %s", value, "id3tag");
    if(!strcmp(tag, "TALB")) id3tag.appendf("Album: %s", value, "id3tag");
    if(!strcmp(tag, "TBPM")) id3tag.appendf("BeatsPerMinute: %s", value, "id3tag");
    if(!strcmp(tag, "TCMP")) id3tag.appendf("Compilation: %s", value, "id3tag");
    if(!strcmp(tag, "TCOM")) id3tag.appendf("Composer: %s", value, "id3tag");
    if(!strcmp(tag, "TCON")) id3tag.appendf("ContentType: %s", value, "id3tag");
    if(!strcmp(tag, "TCOP")) id3tag.appendf("Copyright: %s", value, "id3tag");
    if(!strcmp(tag, "TDAT")) id3tag.appendf("Date: %s", value, "id3tag");
    if(!strcmp(tag, "TDOR")) id3tag.appendf("Original Release Year: %s", value, "id3tag");
    if(!strcmp(tag, "TDRC")) id3tag.appendf("Publication date: %s", value, "id3tag");
    if(!strcmp(tag, "TEXT")) id3tag.appendf("Lyricist: %s", value, "id3tag");
    if(!strcmp(tag, "TIME")) id3tag.appendf("Time: %s", value, "id3tag");
    if(!strcmp(tag, "TIT1")) id3tag.appendf("Grouping: %s", value, "id3tag");
    if(!strcmp(tag, "TIT2")) id3tag.appendf("Title: %s", value, "id3tag");
    if(!strcmp(tag, "TIT3")) id3tag.appendf("Subtitle: %s", value, "id3tag");
    if(!strcmp(tag, "TLAN")) id3tag.appendf("Language: %s", value, "id3tag");
    if(!strcmp(tag, "TLEN")) id3tag.appendf("Length (ms): %s", value, "id3tag");
    if(!strcmp(tag, "TMED")) id3tag.appendf("Media: %s", value, "id3tag");
    if(!strcmp(tag, "TOAL")) id3tag.appendf("OriginalAlbum: %s", value, "id3tag");
    if(!strcmp(tag, "TOPE")) id3tag.appendf("OriginalArtist: %s", value, "id3tag");
    if(!strcmp(tag, "TORY")) id3tag.appendf("OriginalReleaseYear: %s", value, "id3tag");
    if(!strcmp(tag, "TPE1")) id3tag.appendf("Artist: %s", value, "id3tag");
    if(!strcmp(tag, "TPE2")) id3tag.appendf("Band: %s", value, "id3tag");
    if(!strcmp(tag, "TPE3")) id3tag.appendf("Conductor: %s", value, "id3tag");
    if(!strcmp(tag, "TPE4")) id3tag.appendf("InterpretedBy: %s", value, "id3tag");
    if(!strcmp(tag, "TPOS")) id3tag.appendf("PartOfSet: %s", value, "id3tag");
    if(!strcmp(tag, "TPUB")) id3tag.appendf("Publisher: %s", value, "id3tag");
    if(!strcmp(tag, "TRCK")) id3tag.appendf("Track: %s", value, "id3tag");
    if(!strcmp(tag, "TSSE")) id3tag.appendf("SettingsForEncoding: %s", value, "id3tag");
    if(!strcmp(tag, "TRDA")) id3tag.appendf("RecordingDates: %s", value, "id3tag");
    if(!strcmp(tag, "TXXX")) id3tag.appendf("UserDefinedText: %s", value, "id3tag");
    if(!strcmp(tag, "TYER")) id3tag.appendf("Year: %s", value, "id3tag");
    if(!strcmp(tag, "USER")) id3tag.appendf("TermsOfUse: %s", value, "id3tag");
    if(!strcmp(tag, "USLT")) id3tag.appendf("Lyrics: %s", value, "id3tag");
    if(!strcmp(tag, "WOAR")) id3tag.appendf("OfficialArtistWebpage: %s", value, "id3tag");
    if(!strcmp(tag, "WXXX")) id3tag.appendf("User defined URL link frame: %s", value, "id3tag");
    if(!strcmp(tag, "XDOR")) id3tag.appendf("OriginalReleaseTime: %s", value, "id3tag");

    if(!id3tag.valid()){log_w("tag not found: %s", tag); return;}
    latinToUTF8(id3tag);
    if(id3tag.contains("?xml")) {
        showstreamtitle(id3tag.get());
        return;
    }
    if(id3tag.strlen()) {
        if(audio_id3data) audio_id3data(id3tag.get());
    }
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::latinToUTF8(ps_ptr<char>& buff, bool UTF8check) {
    // most stations send  strings in UTF-8 but a few sends in latin. To standardize this, all latin strings are
    // converted to UTF-8. If UTF-8 is already present, nothing is done and true is returned.
    // A conversion to UTF-8 extends the string. Therefore it is necessary to know the buffer size. If the converted
    // string does not fit into the buffer, false is returned

    uint16_t pos = 0;
    uint16_t in = 0;
    uint16_t out = 0;
    bool     isUTF8 = true;

    // We cannot detect if a given string (or byte sequence) is a UTF-8 encoded text as for example each and every series
    // of UTF-8 octets is also a valid (if nonsensical) series of Latin-1 (or some other encoding) octets.
    // However not every series of valid Latin-1 octets are valid UTF-8 series. So you can rule out strings that do not conform
    // to the UTF-8 encoding schema:

    if (UTF8check) {
        while (buff[pos] != '\0') {
            if (buff[pos] <= 0x7F) {// 0xxxxxxx: ASCII
                pos++;
            }
            else if ((buff[pos] & 0xE0) == 0xC0) {
                if (pos + 1 >= buff.strlen()) isUTF8 = false;
                // 110xxxxx 10xxxxxx: 2-byte
                if ((buff[pos + 1] & 0xC0) != 0x80) isUTF8 = false;
                if (buff[pos] < 0xC2) isUTF8 = false; // Overlong encoding
                pos += 2;
            }
            else if  ((buff[pos] & 0xF0) == 0xE0) {
                // 1110xxxx 10xxxxxx 10xxxxxx: 3-byte
                if ((buff[pos + 1] & 0xC0) != 0x80 || (buff[pos + 2] & 0xC0) != 0x80) isUTF8 = false;
                if (buff[pos] == 0xE0 && buff[pos + 1 ] < 0xA0) isUTF8 = false; // Overlong
                if (buff[pos] == 0xED && buff[pos + 1] >= 0xA0) isUTF8 = false; // UTF-16 surrogate
                pos += 3;
            }
            else if ((buff[pos] & 0xF8) == 0xF0) {
                // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx: 4-byte
                if ((buff[pos + 1] & 0xC0) != 0x80 ||
                    (buff[pos + 2] & 0xC0) != 0x80 ||
                    (buff[pos + 3] & 0xC0) != 0x80) isUTF8 = false;
                if (buff[pos] == 0xF0 && buff[pos + 1] < 0x90) isUTF8 = false; // Overlong
                if (buff[pos] > 0xF4 || (buff[pos] == 0xF4 && buff[pos + 1] > 0x8F)) isUTF8 = false; // > U+10FFFF
                pos += 4;
            }
            else{
                isUTF8 = false; // Invalid first byte
            }
            if(!isUTF8)break;
        }
        if (isUTF8) return; // is UTF-8, do nothing
    }
    ps_ptr<char> iso8859_1;
    iso8859_1.assign(buff.get());

    // Worst-case: all chars are latin1 > 0x7F became 2 Bytes → max length is twice +1
    std::size_t requiredSize = strlen(iso8859_1.get()) * 2 + 1;

    if (buff.size() < requiredSize) {
        buff.realloc(requiredSize, "utf8_conv");
    }

    // coding into UTF-8
    while (iso8859_1[in] != '\0') {
        if (iso8859_1[in] < 0x80) {
            buff[out++] = iso8859_1[in++];
        } else {
            buff[out++] = (0xC0 | (iso8859_1[in] >> 6));
            buff[out++] = (0x80 | (iso8859_1[in] & 0x3F));
            in++;
        }
    }
    buff[out] = '\0';
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::htmlToUTF8(char* str) { // convert HTML to UTF-8

    typedef struct { // --- EntityMap Definition ---
        const char *name;
        uint32_t codepoint;
    } EntityMap;

    static const EntityMap entities[] = {
        {"amp",   0x0026}, // &
        {"lt",    0x003C}, // <
        {"gt",    0x003E}, // >
        {"quot",  0x0022}, // "
        {"apos",  0x0027}, // '
        {"nbsp",  0x00A0}, // non-breaking space
        {"euro",  0x20AC}, // €
        {"copy",  0x00A9}, // ©
        {"reg",   0x00AE}, // ®
        {"trade", 0x2122}, // ™
        {"hellip",0x2026}, // …
        {"ndash", 0x2013}, // –
        {"mdash", 0x2014}, // —
        {"sect",  0x00A7}, // §
        {"para",  0x00B6}  // ¶
    };

    // --- EntityMap Lookup ---
    auto find_entity = [&](const char *p, uint32_t *codepoint, int *entity_len) {
        for (size_t i = 0; i < sizeof(entities)/sizeof(entities[0]); i++) {
            const char *name = entities[i].name;
            size_t len = strlen(name);
            if (strncmp(p + 1, name, len) == 0 && p[len + 1] == ';') {
                *codepoint = entities[i].codepoint;
                *entity_len = (int)(len + 2); // &name;
                return 1;
            }
        }
        return 0;
    };


    auto codepoint_to_utf8 = [&](uint32_t cp, char *dst) { // Convert a Codepoint (Unicode) to UTF-8, writes in DST, there is number of bytes back
        if (cp <= 0x7F) {
            dst[0] = cp;
            return 1;
        } else if (cp <= 0x7FF) {
            dst[0] = 0xC0 | (cp >> 6);
            dst[1] = 0x80 | (cp & 0x3F);
            return 2;
        } else if (cp <= 0xFFFF) {
            dst[0] = 0xE0 | (cp >> 12);
            dst[1] = 0x80 | ((cp >> 6) & 0x3F);
            dst[2] = 0x80 | (cp & 0x3F);
            return 3;
        } else if (cp <= 0x10FFFF) {
            dst[0] = 0xF0 | (cp >> 18);
            dst[1] = 0x80 | ((cp >> 12) & 0x3F);
            dst[2] = 0x80 | ((cp >> 6) & 0x3F);
            dst[3] = 0x80 | (cp & 0x3F);
            return 4;
        }
        return -1; // invalid Codepoint
    };

    char *p = str;
    while (*p != '\0') {
        if(p[0] == '&'){
            uint32_t cp;
            int consumed;
            if (find_entity(p, &cp, &consumed)) { // looking for entity, such as &copy;
                char utf8[5] = {0};
                int len = codepoint_to_utf8(cp, utf8);
                if (len > 0) {
                    size_t tail_len = strlen(p + consumed);
                    memmove(p + len, p + consumed, tail_len + 1);
                    memcpy(p, utf8, len);
                    p += len;
                    continue;
                }
            }
        }
        if (p[0] == '&' && p[1] == '#') {
            char *endptr;
            uint32_t codepoint = strtol(p + 2, &endptr, 10);

            if (*endptr == ';' && codepoint <= 0x10FFFF) {
                char utf8[5] = {0};
                int utf8_len = codepoint_to_utf8(codepoint, utf8);
                if (utf8_len > 0) {
                //    size_t entity_len = endptr - p + 1;
                    size_t tail_len = strlen(endptr + 1);

                    // Show residual ring to the left
                    memmove(p + utf8_len, endptr + 1, tail_len + 1);  // +1 because of '\0'

                    // Copy UTF-8 characters
                    memcpy(p, utf8, utf8_len);

                    // weiter bei neuem Zeichen
                    p += utf8_len;
                    continue;
                }
            }
        }
        p++;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
size_t Audio::readAudioHeader(uint32_t bytes) {
    size_t bytesReaded = 0;
    if(m_codec == CODEC_WAV) {
        int res = read_WAV_Header(InBuff.getReadPtr(), bytes);
        if(res >= 0) bytesReaded = res;
        else { // error, skip header
            m_controlCounter = 100;
        }
    }
    if(m_codec == CODEC_MP3) {
        int res = read_ID3_Header(InBuff.getReadPtr(), bytes);
        if(res >= 0) bytesReaded = res;
        else { // error, skip header
            m_controlCounter = 100;
        }
    }
    if(m_codec == CODEC_M4A) {
        int res = read_M4A_Header(InBuff.getReadPtr(), bytes);
        if(res >= 0) bytesReaded = res;
        else { // error, skip header
            m_controlCounter = 100;
        }
    }
    if(m_codec == CODEC_AAC) {
        // stream only, no header
        m_audioDataSize = getFileSize();
        m_controlCounter = 100;
    }
    if(m_codec == CODEC_FLAC) {
        int res = read_FLAC_Header(InBuff.getReadPtr(), bytes);
        if(res >= 0) bytesReaded = res;
        else { // error, skip header
            stopSong();
            m_controlCounter = 100;
        }
    }
    if(m_codec == CODEC_OPUS)   { m_controlCounter = 100; }
    if(m_codec == CODEC_VORBIS) { m_controlCounter = 100; }
    if(m_codec == CODEC_OGG)    { m_controlCounter = 100; }
    if(!isRunning()) {
        log_e("Processing stopped due to invalid audio header");
        return 0;
    }
    return bytesReaded;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int Audio::read_WAV_Header(uint8_t* data, size_t len) {
    static size_t   headerSize;
    static uint32_t cs = 0;
    static uint8_t  bts = 0;

    if(m_controlCounter == 0) {
        m_controlCounter++;
        if((*data != 'R') || (*(data + 1) != 'I') || (*(data + 2) != 'F') || (*(data + 3) != 'F')) {
            AUDIO_INFO("file has no RIFF tag");
            headerSize = 0;
            return -1; // false;
        }
        else {
            headerSize = 4;
            return 4; // ok
        }
    }

    if(m_controlCounter == 1) {
        m_controlCounter++;
        cs = (uint32_t)(*data + (*(data + 1) << 8) + (*(data + 2) << 16) + (*(data + 3) << 24) - 8);
        headerSize += 4;
        return 4; // ok
    }

    if(m_controlCounter == 2) {
        m_controlCounter++;
        if((*data != 'W') || (*(data + 1) != 'A') || (*(data + 2) != 'V') || (*(data + 3) != 'E')) {
            AUDIO_INFO("format tag is not WAVE");
            return -1; // false;
        }
        else {
            headerSize += 4;
            return 4;
        }
    }

    if(m_controlCounter == 3) {
        if((*data == 'f') && (*(data + 1) == 'm') && (*(data + 2) == 't')) {
            m_controlCounter++;
            headerSize += 4;
            return 4;
        }
        else {
            headerSize += 4;
            return 4;
        }
    }

    if(m_controlCounter == 4) {
        m_controlCounter++;
        cs = (uint32_t)(*data + (*(data + 1) << 8));
        if(cs > 40) return -1; // false, something going wrong
        bts = cs - 16;         // bytes to skip if fmt chunk is >16
        headerSize += 4;
        return 4;
    }

    if(m_controlCounter == 5) {
        m_controlCounter++;
        uint16_t fc = (uint16_t)(*(data + 0) + (*(data + 1) << 8));                                               // Format code
        uint16_t nic = (uint16_t)(*(data + 2) + (*(data + 3) << 8));                                              // Number of interleaved channels
        uint32_t sr = (uint32_t)(*(data + 4) + (*(data + 5) << 8) + (*(data + 6) << 16) + (*(data + 7) << 24));   // Samplerate
        uint32_t dr = (uint32_t)(*(data + 8) + (*(data + 9) << 8) + (*(data + 10) << 16) + (*(data + 11) << 24)); // Datarate
        uint16_t dbs = (uint16_t)(*(data + 12) + (*(data + 13) << 8));                                            // Data block size
        uint16_t bps = (uint16_t)(*(data + 14) + (*(data + 15) << 8));                                            // Bits per sample

        AUDIO_INFO("FormatCode: %u", fc);
        // AUDIO_INFO("Channel: %u", nic);
        // AUDIO_INFO("SampleRate: %u", sr);
        AUDIO_INFO("DataRate: %lu", (long unsigned int)dr);
        AUDIO_INFO("DataBlockSize: %u", dbs);
        AUDIO_INFO("BitsPerSample: %u", bps);

        if((bps != 8) && (bps != 16)) {
            AUDIO_INFO("BitsPerSample is %u,  must be 8 or 16", bps);
            stopSong();
            return -1;
        }
        if((nic != 1) && (nic != 2)) {
            AUDIO_INFO("num channels is %u,  must be 1 or 2", nic);
            stopSong();
            return -1;
        }
        if(fc != 1) {
            AUDIO_INFO("format code is not 1 (PCM)");
            stopSong();
            return -1; // false;
        }
        setBitsPerSample(bps);
        setChannels(nic);
        setSampleRate(sr);
        setBitrate(nic * sr * bps);
        //    AUDIO_INFO("BitRate: %u", m_bitRate);
        headerSize += 16;
        return 16; // ok
    }

    if(m_controlCounter == 6) {
        m_controlCounter++;
        headerSize += bts;
        return bts; // skip to data
    }

    if(m_controlCounter == 7) {
        if((*(data + 0) == 'd') && (*(data + 1) == 'a') && (*(data + 2) == 't') && (*(data + 3) == 'a')) {
            m_controlCounter++;
            //    vTaskDelay(30);
            headerSize += 4;
            return 4;
        }
        else {
            headerSize++;
            return 1;
        }
    }

    if(m_controlCounter == 8) {
        m_controlCounter++;
        size_t cs = *(data + 0) + (*(data + 1) << 8) + (*(data + 2) << 16) + (*(data + 3) << 24); // read chunkSize
        headerSize += 4;
        if(m_dataMode == AUDIO_LOCALFILE) m_contentlength = getFileSize();
        if(cs) { m_audioDataSize = cs - 44; }
        else { // sometimes there is nothing here
            if(m_dataMode == AUDIO_LOCALFILE) m_audioDataSize = getFileSize() - headerSize;
            if(m_streamType == ST_WEBFILE) m_audioDataSize = m_contentlength - headerSize;
        }
        AUDIO_INFO("Audio-Length: %u", m_audioDataSize);
        return 4;
    }
    m_controlCounter = 100; // header succesfully read
    m_audioDataStart = headerSize;
    return 0;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int Audio::read_FLAC_Header(uint8_t* data, size_t len) {
    static size_t   headerSize;
    static size_t   retvalue = 0;
    static bool     f_lastMetaBlock = false;
    static uint32_t picPos = 0;
    static uint32_t picLen = 0;

    if(retvalue) {
        if(retvalue > len) { // if returnvalue > bufferfillsize
            if(len > InBuff.getMaxBlockSize()) len = InBuff.getMaxBlockSize();
            retvalue -= len; // and wait for more bufferdata
            return len;
        }
        else {
            size_t tmp = retvalue;
            retvalue = 0;
            return tmp;
        }
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == FLAC_BEGIN) { // init
        headerSize = 0;
        retvalue = 0;
        m_audioDataStart = 0;
        picPos = 0;
        picLen = 0;
        f_lastMetaBlock = false;
        m_controlCounter = FLAC_MAGIC;
        if(m_dataMode == AUDIO_LOCALFILE) {
            m_contentlength = getFileSize();
            AUDIO_INFO("Content-Length: %lu", (long unsigned int)m_contentlength);
        }
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == FLAC_MAGIC) {            /* check MAGIC STRING */
        if(specialIndexOf(data, "OggS", 10) == 0) { // is ogg
            headerSize = 0;
            retvalue = 0;
            m_controlCounter = FLAC_OKAY;
            return 0;
        }
        if(specialIndexOf(data, "fLaC", 10) != 0) {
            log_e("Magic String 'fLaC' not found in header");
            stopSong();
            return -1;
        }
        m_controlCounter = FLAC_MBH; // METADATA_BLOCK_HEADER
        headerSize = 4;
        retvalue = 4;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == FLAC_MBH) { /* METADATA_BLOCK_HEADER */
        uint8_t blockType = *data;
        if(!f_lastMetaBlock) {
            if(blockType & 128) { f_lastMetaBlock = true; }
            blockType &= 127;
            if(blockType == 0) m_controlCounter = FLAC_SINFO;
            if(blockType == 1) m_controlCounter = FLAC_PADDING;
            if(blockType == 2) m_controlCounter = FLAC_APP;
            if(blockType == 3) m_controlCounter = FLAC_SEEK;
            if(blockType == 4) m_controlCounter = FLAC_VORBIS;
            if(blockType == 5) m_controlCounter = FLAC_CUESHEET;
            if(blockType == 6) m_controlCounter = FLAC_PICTURE;
            headerSize += 1;
            retvalue = 1;
            return 0;
        }
        m_controlCounter = FLAC_OKAY;
        m_audioDataStart = headerSize;
        m_audioDataSize = m_contentlength - m_audioDataStart;
        FLACSetRawBlockParams(m_flacNumChannels, m_flacSampleRate, m_flacBitsPerSample, m_flacTotalSamplesInStream, m_audioDataSize);
#ifndef AUDIO_NO_SD_FS
        if(picLen) {
            size_t pos = m_audiofile.position();
            if(audio_id3image) audio_id3image(m_audiofile, picPos, picLen);
            m_audiofile.seek(pos); // the filepointer could have been changed by the user, set it back
        }
#endif
        AUDIO_INFO("Audio-Length: %u", m_audioDataSize);
        retvalue = 0;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == FLAC_SINFO) { /* Stream info block */
        size_t l = bigEndian(data, 3);
        vTaskDelay(2);
        m_flacMaxBlockSize = bigEndian(data + 5, 2);
        AUDIO_INFO("FLAC maxBlockSize: %u", m_flacMaxBlockSize);
        vTaskDelay(2);
        m_flacMaxFrameSize = bigEndian(data + 10, 3);
        if(m_flacMaxFrameSize) { AUDIO_INFO("FLAC maxFrameSize: %u", m_flacMaxFrameSize); }
        else { AUDIO_INFO("FLAC maxFrameSize: N/A"); }
        if(m_flacMaxFrameSize > InBuff.getMaxBlockSize()) {
            log_e("FLAC maxFrameSize too large!");
            stopSong();
            return -1;
        }
        //        InBuff.changeMaxBlockSize(m_flacMaxFrameSize);
        vTaskDelay(2);
        uint32_t nextval = bigEndian(data + 13, 3);
        m_flacSampleRate = nextval >> 4;
        AUDIO_INFO("FLAC sampleRate: %lu", (long unsigned int)m_flacSampleRate);
        vTaskDelay(2);
        m_flacNumChannels = ((nextval & 0x06) >> 1) + 1;
        AUDIO_INFO("FLAC numChannels: %u", m_flacNumChannels);
        vTaskDelay(2);
        uint8_t bps = (nextval & 0x01) << 4;
        bps += (*(data + 16) >> 4) + 1;
        m_flacBitsPerSample = bps;
        if((bps != 8) && (bps != 16)) {
            log_e("bits per sample must be 8 or 16, is %i", bps);
            stopSong();
            return -1;
        }
        AUDIO_INFO("FLAC bitsPerSample: %u", m_flacBitsPerSample);
        m_flacTotalSamplesInStream = bigEndian(data + 17, 4);
        if(m_flacTotalSamplesInStream) { AUDIO_INFO("total samples in stream: %lu", (long unsigned int)m_flacTotalSamplesInStream); }
        else { AUDIO_INFO("total samples in stream: N/A"); }
        if(bps != 0 && m_flacTotalSamplesInStream) { AUDIO_INFO("audio file duration: %lu seconds", (long unsigned int)m_flacTotalSamplesInStream / (long unsigned int)m_flacSampleRate); }
        m_controlCounter = FLAC_MBH; // METADATA_BLOCK_HEADER
        retvalue = l + 3;
        headerSize += retvalue;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == FLAC_PADDING) { /* PADDING */
        size_t l = bigEndian(data, 3);
        m_controlCounter = FLAC_MBH;
        retvalue = l + 3;
        headerSize += retvalue;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == FLAC_APP) { /* APPLICATION */
        size_t l = bigEndian(data, 3);
        m_controlCounter = FLAC_MBH;
        retvalue = l + 3;
        headerSize += retvalue;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == FLAC_SEEK) { /* SEEKTABLE */
        size_t l = bigEndian(data, 3);
        m_controlCounter = FLAC_MBH;
        retvalue = l + 3;
        headerSize += retvalue;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == FLAC_VORBIS) { /* VORBIS COMMENT */ // field names
        ps_ptr<char>vendorString = {};
        size_t vendorLength = bigEndian(data, 3);
        size_t idx = 0;
        data += 3; idx += 3;
        size_t vendorStringLength = data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
        if(vendorStringLength) {data += 4; idx += 4;}
        if(vendorStringLength > 495) vendorStringLength = 495; // guard
        vendorString.assign((const char*)data, vendorStringLength, "vendorString");
        vendorString.insert("VENDOR_STRING: ", 0);
        if(audio_id3data) audio_id3data(vendorString.get());
        data += vendorStringLength; idx += vendorStringLength;
        size_t commentListLength = data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
        data += 4; idx += 4;

        for(int i = 0; i < commentListLength; i++) {
            (void)i;
            size_t commentLength = data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
            data += 4; idx += 4;
            if(commentLength) { // guard
                vendorString.assign((const char*)data , commentLength, "vendorString");
                if(audio_id3data) audio_id3data(vendorString.get());
            }
            data += commentLength; idx += commentLength;
            if(idx > vendorLength + 3) {log_e("VORBIS COMMENT section is too long");}
        }
        m_controlCounter = FLAC_MBH;
        retvalue = vendorLength + 3;
        headerSize += retvalue;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == FLAC_CUESHEET) { /* CUESHEET */
        size_t l = bigEndian(data, 3);
        m_controlCounter = FLAC_MBH;
        retvalue = l + 3;
        headerSize += retvalue;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == FLAC_PICTURE) { /* PICTURE */
        picLen = bigEndian(data, 3);
        picPos = headerSize;
        // log_w("FLAC PICTURE, size %i, pos %i", picLen, picPos);
        m_controlCounter = FLAC_MBH;
        retvalue = picLen + 3;
        headerSize += retvalue;
        return 0;
    }
    return 0;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int Audio::read_ID3_Header(uint8_t* data, size_t len) {

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == 0) { /* read ID3 tag and ID3 header size */
        m_controlCounter++;

        ID3Hdr.id3Size = 0;
        ID3Hdr.totalId3Size = 0; // if we have more header, id3_1_size + id3_2_size + ....
        ID3Hdr.remainingHeaderBytes = 0;
        ID3Hdr.universal_tmp = 0;
        ID3Hdr.ID3version = 0;
        ID3Hdr.ehsz = 0;
        ID3Hdr.framesize = 0;
        ID3Hdr.compressed = false;
        ID3Hdr.SYLT_seen = false;
        ID3Hdr.SYLT_size = 0;
        ID3Hdr.SYLT_pos = 0;
        ID3Hdr.numID3Header = 0;
        ID3Hdr.iBuffSize = 4096;
        ID3Hdr.iBuff.alloc(ID3Hdr.iBuffSize + 10, "iBuff");
        memset(ID3Hdr.tag, 0, sizeof(ID3Hdr.tag));
        memset(ID3Hdr.APIC_size, 0, sizeof(ID3Hdr.APIC_size));
        memset(ID3Hdr.APIC_pos, 0, sizeof(ID3Hdr.APIC_pos));
        memset(ID3Hdr.tag, 0, sizeof(ID3Hdr.tag));

        if(m_dataMode == AUDIO_LOCALFILE) {
            ID3Hdr.ID3version = 0;
            m_contentlength = getFileSize();
            AUDIO_INFO("Content-Length: %lu", (long unsigned int)m_contentlength);
        }

        ID3Hdr.SYLT_seen = false;
        ID3Hdr.remainingHeaderBytes = 0;
        ID3Hdr.ehsz = 0;
        if(specialIndexOf(data, "ID3", 4) != 0) { // ID3 not found
            if(!m_f_m3u8data) {AUDIO_INFO("file has no ID3 tag, skip metadata");}
            m_audioDataSize = m_contentlength;
            if(!m_f_m3u8data) AUDIO_INFO("Audio-Length: %u", m_audioDataSize);
            return -1; // error, no ID3 signature found
        }
        ID3Hdr.ID3version = *(data + 3);
        switch(ID3Hdr.ID3version) {
            case 2:
                m_f_unsync = (*(data + 5) & 0x80);
                m_f_exthdr = false;
                break;
            case 3:
            case 4:
                m_f_unsync = (*(data + 5) & 0x80); // bit7
                m_f_exthdr = (*(data + 5) & 0x40); // bit6 extended header
                break;
        };
        ID3Hdr.id3Size = bigEndian(data + 6, 4, 7); //  ID3v2 size  4 * %0xxxxxxx (shift left seven times!!)
        ID3Hdr.id3Size += 10;

        // Every read from now may be unsync'd
        if(!m_f_m3u8data) AUDIO_INFO("ID3 framesSize: %i", ID3Hdr.id3Size);
        if(!m_f_m3u8data) AUDIO_INFO("ID3 version: 2.%i", ID3Hdr.ID3version);

        if(ID3Hdr.ID3version == 2) { m_controlCounter = 10; }
        ID3Hdr.remainingHeaderBytes = ID3Hdr.id3Size;
        m_ID3Size = ID3Hdr.id3Size;
        ID3Hdr.remainingHeaderBytes -= 10;

        return 10;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == 1) { // compute extended header size if exists
        m_controlCounter++;
        if(m_f_exthdr) {
            AUDIO_INFO("ID3 extended header");
            ID3Hdr.ehsz = bigEndian(data, 4);
            ID3Hdr.remainingHeaderBytes -= 4;
            ID3Hdr.ehsz -= 4;
            return 4;
        }
        else {
            if(!m_f_m3u8data) AUDIO_INFO("ID3 normal frames");
            return 0;
        }
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == 2) { // skip extended header if exists
        if(ID3Hdr.ehsz > len) {
            ID3Hdr.ehsz -= len;
            ID3Hdr.remainingHeaderBytes -= len;
            return len;
        } // Throw it away
        else {
            m_controlCounter++;
            ID3Hdr.remainingHeaderBytes -= ID3Hdr.ehsz;
            return ID3Hdr.ehsz;
        } // Throw it away
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == 3) { // read a ID3 frame, get the tag
        if(ID3Hdr.remainingHeaderBytes == 0) {
            m_controlCounter = 99;
            return 0;
        }
        m_controlCounter++;
        ID3Hdr.frameid[0] = *(data + 0);
        ID3Hdr.frameid[1] = *(data + 1);
        ID3Hdr.frameid[2] = *(data + 2);
        ID3Hdr.frameid[3] = *(data + 3);
        ID3Hdr.frameid[4] = 0;
        for(uint8_t i = 0; i < 4; i++) ID3Hdr.tag[i] = ID3Hdr.frameid[i]; // tag = frameid

        ID3Hdr.remainingHeaderBytes -= 4;
        if(ID3Hdr.frameid[0] == 0 && ID3Hdr.frameid[1] == 0 && ID3Hdr.frameid[2] == 0 && ID3Hdr.frameid[3] == 0) {
            // We're in padding
            m_controlCounter = 98; // all ID3 metadata processed
        }
        return 4;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == 4) { // get the frame size
        m_controlCounter = 6;

        if(ID3Hdr.ID3version == 4) {
            ID3Hdr.framesize = bigEndian(data, 4, 7); // << 7
        }
        else {
            ID3Hdr.framesize = bigEndian(data, 4); // << 8
        }
        ID3Hdr.remainingHeaderBytes -= 4;
        uint8_t frameFlag_0 = *(data + 4);
        uint8_t frameFlag_1 = *(data + 5);
        (void)frameFlag_0;
        ID3Hdr.remainingHeaderBytes--;
        ID3Hdr.compressed = frameFlag_1 & 0x80; // Frame is compressed using [#ZLIB zlib] with 4 bytes for 'decompressed
        ID3Hdr.remainingHeaderBytes--;
        uint32_t decompsize = 0;
        if(ID3Hdr.compressed) {
            // log_i("iscompressed");
            decompsize = bigEndian(data + 6, 4);
            ID3Hdr.remainingHeaderBytes -= 4;
            (void)decompsize;
            // log_i("decompsize=%u", decompsize);
            return 6 + 4;
        }
        return 6;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == 5) { // If the frame is larger than ID3Hdr.framesize, skip the rest
        if(ID3Hdr.framesize > len) {
            ID3Hdr.framesize -= len;
            ID3Hdr.remainingHeaderBytes -= len;
            return len;
        }
        else {
            m_controlCounter = 3; // check next frame
            ID3Hdr.remainingHeaderBytes -= ID3Hdr.framesize;
            return ID3Hdr.framesize;
        }
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == 6) { // Read the value
        m_controlCounter = 5;   // only read 256 bytes
        uint8_t textEncodingByte = *(data + 0);  // ID3v2 Text-Encoding-Byte
        // $00 – ISO-8859-1 (LATIN-1, Identical to ASCII for values smaller than 0x80).
        // $01 – UCS-2 encoded Unicode with BOM (Byte Order Mark), in ID3v2.2 and ID3v2.3.
        // $02 – UTF-16BE encoded Unicode without BOM (Byte Order Mark) , in ID3v2.4.
        // $03 – UTF-8 encoded Unicode, in ID3v2.4.

        if(startsWith(ID3Hdr.tag, "APIC")) { // a image embedded in file, passing it to external function
        //    if(m_dataMode == AUDIO_LOCALFILE) {
#ifndef AUDIO_NO_SD_FS
                ID3Hdr.APIC_pos[ID3Hdr.numID3Header] = ID3Hdr.totalId3Size + ID3Hdr.id3Size - ID3Hdr.remainingHeaderBytes;
                ID3Hdr.APIC_size[ID3Hdr.numID3Header] = ID3Hdr.framesize;
                //    log_e("APIC_pos %i APIC_size %i", APIC_pos[numID3Header], APIC_size[numID3Header]);
#endif	// AUDIO_NO_SD_FS        
        //    }
            return 0;
        }

        if( // any lyrics embedded in file, passing it to external function
            startsWith(ID3Hdr.tag, "SYLT") || startsWith(ID3Hdr.tag, "TXXX") || startsWith(ID3Hdr.tag, "USLT")) {
            if(m_dataMode == AUDIO_LOCALFILE) {
#ifndef AUDIO_NO_SD_FS	
                ID3Hdr.SYLT_seen = true;
                ID3Hdr.SYLT_pos = ID3Hdr.id3Size - ID3Hdr.remainingHeaderBytes;
                ID3Hdr.SYLT_size = ID3Hdr.framesize;
#endif	// AUDIO_NO_SD_FS
            }
            return 0;
        }

        if( // proprietary not standard information
            startsWith(ID3Hdr.tag, "PRIV")) {
                ;//log_e("PRIV");
                return 0;
        }



        if(ID3Hdr.framesize == 0) return 0;

        size_t fs = ID3Hdr.framesize; // fs = size of the frame data field as read from header
        size_t bytesToCopy = fs;

        if (bytesToCopy >= ID3Hdr.iBuffSize) { // <= oder >= hier ist wichtig!
            bytesToCopy = ID3Hdr.iBuffSize - 1; // Sicherstellen, dass ein Null-Terminator passt
        }
        size_t textDataLength = 0;
        if (bytesToCopy > 0) { // Nur wenn überhaupt Daten da sind, die wir kürzen können
            textDataLength = bytesToCopy - 1; // Dies ist die Anzahl der zu kopierenden TEXT-Bytes
        }
        for(int i = 0; i < textDataLength; i++) {
            ID3Hdr.iBuff[i] = *(data + i + 1); // Überspringt das erste Byte (Encoding)
        }
        ID3Hdr.iBuff[textDataLength] = 0;
        ID3Hdr.framesize -= fs;
        ID3Hdr.remainingHeaderBytes -= fs;
        uint16_t dataLength = fs - 1;

        if(textEncodingByte == 0){  // latin
            latinToUTF8(ID3Hdr.iBuff, false);
            showID3Tag(ID3Hdr.tag, ID3Hdr.iBuff.get());
        }

        if(textEncodingByte == 1  && dataLength > 1) { // UTF16 with BOM
            int8_t data_start = 0;
            if(startsWith(ID3Hdr.tag, "COMM")){ // language code
                ID3Hdr.lang[0] = ID3Hdr.iBuff[0];
                ID3Hdr.lang[1] = ID3Hdr.iBuff[1];
                ID3Hdr.lang[2] = ID3Hdr.iBuff[2];
                ID3Hdr.lang[3] = '\0';
                data_start += 3;
                // log_w("language code: %s", ID3Hdr.lang);
                ID3Hdr.byteOrderMark = static_cast<unsigned char>(ID3Hdr.iBuff[data_start]) == 0xFE && static_cast<unsigned char>(ID3Hdr.iBuff[data_start]) == 0xFF;
                data_start += 2;
                ID3Hdr.contentDescriptorTerminator_0 = ID3Hdr.iBuff[data_start];
                ID3Hdr.contentDescriptorTerminator_1 = ID3Hdr.iBuff[data_start + 1];
                ID3Hdr.textStringTerminator_0        = ID3Hdr.iBuff[data_start + 2];
                ID3Hdr.textStringTerminator_1        = ID3Hdr.iBuff[data_start + 3];
                data_start += 4;
            }
            else{
                ID3Hdr.byteOrderMark = static_cast<unsigned char>(ID3Hdr.iBuff[data_start]) == 0xFE && static_cast<unsigned char>(ID3Hdr.iBuff[data_start]) == 0xFF;
                data_start += 2;
            }

            std::u16string utf16_string;
            for (size_t i = data_start; i < dataLength; i += 2) {
                char16_t wchar;
                if(ID3Hdr.byteOrderMark)  wchar = (static_cast<unsigned char>(ID3Hdr.iBuff[i]) << 8) | static_cast<unsigned char>(ID3Hdr.iBuff[i + 1]);
                else                      wchar = (static_cast<unsigned char>(ID3Hdr.iBuff[i + 1]) << 8) | static_cast<unsigned char>(ID3Hdr.iBuff[i]);
                utf16_string.push_back(wchar);
            }

            std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
            showID3Tag(ID3Hdr.tag, converter.to_bytes(utf16_string).c_str());
        }

        if(textEncodingByte == 2 && dataLength > 1) { // UTF16BE

            std::u16string utf16_string;
            for (size_t i = 0; i < dataLength; i += 2) {
                char16_t  wchar = (static_cast<unsigned char>(ID3Hdr.iBuff[i]) << 8) | static_cast<unsigned char>(ID3Hdr.iBuff[i + 1]);
                utf16_string.push_back(wchar);
            }

            std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
            showID3Tag(ID3Hdr.tag, converter.to_bytes(utf16_string).c_str());
        }

        if(textEncodingByte == 3) { // utf8
            showID3Tag(ID3Hdr.tag, ID3Hdr.iBuff.get());
        }
        return fs;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    // --- section V2.2 only , greater Vers above ----
    // see https://mutagen-specs.readthedocs.io/en/latest/id3/id3v2.2.html
    if(m_controlCounter == 10) { // frames in V2.2, 3bytes identifier, 3bytes size descriptor

        if(ID3Hdr.universal_tmp > 0) {
            if(ID3Hdr.universal_tmp > len) {
                ID3Hdr.universal_tmp -= len;
                return len;
            } // Throw it away
            else {
                uint32_t t = ID3Hdr.universal_tmp;
                ID3Hdr.universal_tmp = 0;
                return t;
            } // Throw it away
        }

        ID3Hdr.frameid[0] = *(data + 0);
        ID3Hdr.frameid[1] = *(data + 1);
        ID3Hdr.frameid[2] = *(data + 2);
        ID3Hdr.frameid[3] = 0;
        for(uint8_t i = 0; i < 4; i++) ID3Hdr.tag[i] = ID3Hdr.frameid[i]; // tag = frameid
        ID3Hdr.remainingHeaderBytes -= 3;
        size_t dataLen = bigEndian(data + 3, 3);
        ID3Hdr.universal_tmp = dataLen;
        ID3Hdr.remainingHeaderBytes -= 3;
        char value[256];
        if(dataLen > 249) { dataLen = 249; }
        memcpy(value, (data + 7), dataLen);
        value[dataLen + 1] = 0;
        if(startsWith(ID3Hdr.tag, "PIC")) { // image embedded in header
            if(m_dataMode == AUDIO_LOCALFILE) {
           #ifndef AUDIO_NO_SD_FS
                ID3Hdr.APIC_pos[ID3Hdr.numID3Header] = ID3Hdr.id3Size - ID3Hdr.remainingHeaderBytes;
                ID3Hdr.APIC_size[ID3Hdr.numID3Header] = ID3Hdr.universal_tmp;
                // log_i("Attached picture seen at pos %d length %d", APIC_pos[0], APIC_size[0]);
            #endif
            }
        }
        else if(startsWith(ID3Hdr.tag, "SLT")) { // lyrics embedded in header
            if(m_dataMode == AUDIO_LOCALFILE) {
#ifndef AUDIO_NO_SD_FS
                ID3Hdr.SYLT_seen = true; // #460
                ID3Hdr.SYLT_pos = ID3Hdr.id3Size - ID3Hdr.remainingHeaderBytes;
                ID3Hdr.SYLT_size = ID3Hdr.universal_tmp;
                // log_i("Attached lyrics seen at pos %d length %d", SYLT_pos, SYLT_size);
#endif
            }
        }
        else { showID3Tag(ID3Hdr.tag, value);}
        ID3Hdr.remainingHeaderBytes -= ID3Hdr.universal_tmp;
        ID3Hdr.universal_tmp -= dataLen;

        if(dataLen == 0) m_controlCounter = 98;
        if(ID3Hdr.remainingHeaderBytes == 0) m_controlCounter = 98;

        return 3 + 3 + dataLen;
    }
    // -- end section V2.2 -----------

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == 98) { // skip all ID3 metadata (mostly spaces)
        if(ID3Hdr.remainingHeaderBytes > len) {
            ID3Hdr.remainingHeaderBytes -= len;
            return len;
        } // Throw it away
        else {
            m_controlCounter = 99;
            return ID3Hdr.remainingHeaderBytes;
        } // Throw it away
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == 99) { //  exist another ID3tag?
        m_audioDataStart += ID3Hdr.id3Size;
        //    vTaskDelay(30);
        if((*(data + 0) == 'I') && (*(data + 1) == 'D') && (*(data + 2) == '3')) {
            m_controlCounter = 0;
#ifndef AUDIO_NO_SD_FS 
            ID3Hdr.numID3Header++;
#endif  // AUDIO_NO_SD_FS  
            ID3Hdr.totalId3Size += ID3Hdr.id3Size;
            return 0;
        }
        else {
            m_controlCounter = 100; // ok
            m_audioDataSize = m_contentlength - m_audioDataStart;
            if(!m_f_m3u8data) AUDIO_INFO("Audio-Length: %u", m_audioDataSize);
 #ifndef AUDIO_NO_SD_FS
            if(ID3Hdr.APIC_pos[0] && audio_id3image) { // if we have more than one APIC, output the first only
                size_t pos = m_audiofile.position();
                audio_id3image(m_audiofile, ID3Hdr.APIC_pos[0], ID3Hdr.APIC_size[0]);
                m_audiofile.seek(pos); // the filepointer could have been changed by the user, set it back
            }
            if(ID3Hdr.SYLT_seen && audio_id3lyrics) {
                size_t pos = m_audiofile.position();
                audio_id3lyrics(m_audiofile, ID3Hdr.SYLT_pos, ID3Hdr.SYLT_size);
                m_audiofile.seek(pos); // the filepointer could have been changed by the user, set it back
            }
 #endif
            ID3Hdr.numID3Header = 0;
            ID3Hdr.totalId3Size = 0;
            for(int i = 0; i < 3; i++) ID3Hdr.APIC_pos[i] = 0;  // delete all
            for(int i = 0; i < 3; i++) ID3Hdr.APIC_size[i] = 0; // delete all
            ID3Hdr.iBuff.reset();
            return 0;
        }
    }
    return 0;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int Audio::read_M4A_Header(uint8_t* data, size_t len) {

    // Lambda function for Variant length determination
    auto parse_variant_length = [](uint8_t *&ptr) -> int {
        int length = 0;
        do {
            length = (length << 7) | (*ptr & 0x7F);  // Read the lower 7 bits of the current byte
        } while (*(ptr++) & 0x80);  //Increment the pointer after each byte
        return length;
    };

    /*
         ftyp
           | - moov  -> trak -> ... -> mp4a contains raw block parameters
           |    L... -> ilst  contains artist, composer ....
         free (optional) // jump to another atoms at the end of mdat
           |
         mdat contains the audio data                                                      */

    if(m_controlCounter == M4A_BEGIN) m4aHdr.retvalue = 0;

    if(m4aHdr.retvalue) {
        if(len > InBuff.getMaxBlockSize()) len = InBuff.getMaxBlockSize();
        if(m4aHdr.retvalue > len) { // if returnvalue > bufferfillsize
            m4aHdr.retvalue -= len; // and wait for more bufferdata
            m4aHdr.cnt += len;
            return len;
        }
        else {
            size_t tmp = m4aHdr.retvalue;
            m4aHdr.retvalue = 0;
            m4aHdr.cnt += tmp;
            m4aHdr.cnt = 0;
            return tmp;
        }
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == M4A_BEGIN) { // init
        m4aHdr.headerSize = 0;
        m4aHdr.retvalue = 0;
        m4aHdr.atomsize = 0;
        m4aHdr.audioDataPos = 0;
        m4aHdr.cnt = 0;
        m4aHdr.picPos = 0;
        m4aHdr.picLen = 0;
        m_controlCounter = M4A_FTYP;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == M4A_FTYP) { /* check_m4a_file */
        m4aHdr.atomsize = bigEndian(data, 4); // length of first atom
        if(specialIndexOf(data, "ftyp", 10) != 4) {
            log_e("atom 'ftyp' not found in header");
            stopSong();
            return -1;
        }
        int m4a = specialIndexOf(data, "M4A ", 20);
        int isom = specialIndexOf(data, "isom", 20);
        int mp42 = specialIndexOf(data, "mp42", 20);

        if((m4a != 8) && (isom != 8) && (mp42 != 8)) {
            log_e("subtype 'MA4 ', 'isom' or 'mp42' expected, but found '%s '", (data + 8));
            stopSong();
            return -1;
        }

        m_controlCounter = M4A_CHK;
        m4aHdr.retvalue = m4aHdr.atomsize;
        m4aHdr.headerSize = m4aHdr.atomsize;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == M4A_CHK) {  /* check  Tag */
        m4aHdr.atomsize = bigEndian(data, 4); // length of this atom
        if(specialIndexOf(data, "moov", 10) == 4) {
            m_controlCounter = M4A_MOOV;
            return 0;
        }
        else if(specialIndexOf(data, "free", 10) == 4) {
            m4aHdr.retvalue = m4aHdr.atomsize;
            m4aHdr.headerSize += m4aHdr.atomsize;
            return 0;
        }
        else if(specialIndexOf(data, "mdat", 10) == 4) {
            m_controlCounter = M4A_MDAT;
            return 0;
        }
        else {
            char atomName[5] = {0};
            (void)atomName;
            atomName[0] = *data;
            atomName[1] = *(data + 1);
            atomName[2] = *(data + 2);
            atomName[3] = *(data + 3);
            atomName[4] = 0;

            // log_i("atom %s found", atomName);

            m4aHdr.retvalue = m4aHdr.atomsize;
            m4aHdr.headerSize += m4aHdr.atomsize;
            return 0;
        }
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == M4A_MOOV) { // moov
        // we are looking for track and ilst
        if(specialIndexOf(data, "trak", len) > 0) {
            int offset = specialIndexOf(data, "trak", len);
            m4aHdr.retvalue = offset;
            m4aHdr.atomsize -= offset;
            m4aHdr.headerSize += offset;
            m_controlCounter = M4A_TRAK;
            return 0;
        }
        if(specialIndexOf(data, "ilst", len) > 0) {
            int offset = specialIndexOf(data, "ilst", len);
            m4aHdr.retvalue = offset;
            m4aHdr.atomsize -= offset;
            m4aHdr.headerSize += offset;
            m_controlCounter = M4A_ILST;
            return 0;
        }
        m_controlCounter = M4A_CHK;
        m4aHdr.headerSize += m4aHdr.atomsize;
        m4aHdr.retvalue = m4aHdr.atomsize;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == M4A_TRAK) { // trak
        if(specialIndexOf(data, "esds", len) > 0) {
            int      esds = specialIndexOf(data, "esds", len); // Packaging/Encapsulation And Setup Data
            uint8_t* pos = data + esds;
            pos += 8; // skip header

            if(*pos == 0x03) {;} // Found ES Descriptor (Tag: 0x03)
            pos++;
            int es_descriptor_len = parse_variant_length(pos); (void)es_descriptor_len;
            uint16_t es_id = (pos[0] << 8) | pos[1]; (void)es_id;
            uint8_t flags = pos[2]; (void)flags;
            pos += 3; // skip ES Descriptor data

            if (*pos == 0x04) {;}
            pos++; // skip tag

            int decoder_config_len = parse_variant_length(pos); (void)decoder_config_len;
            uint8_t object_type_indication = pos[0];

            if     (object_type_indication == (uint8_t)0x40) { AUDIO_INFO("AudioType: MPEG4 / Audio"); } // ObjectTypeIndication
            else if(object_type_indication == (uint8_t)0x66) { AUDIO_INFO("AudioType: MPEG2 / Audio"); }
            else if(object_type_indication == (uint8_t)0x69) { AUDIO_INFO("AudioType: MPEG2 / Audio Part 3"); } // Backward Compatible Audio
            else if(object_type_indication == (uint8_t)0x6B) { AUDIO_INFO("AudioType: MPEG1 / Audio"); }
            else { AUDIO_INFO("unknown Audio Type %x", object_type_indication); }

            pos++;
            uint8_t streamType = *pos >> 2;   // The upper 6 Bits are the StreamType
            if(streamType != 0x05) { log_e("Streamtype is not audio!"); }
            pos += 4; // ST + BufferSizeDB.
            uint32_t maxBr = bigEndian(pos, 4); // max bitrate
            pos += 4;
            AUDIO_INFO("max bitrate: %lu", (long unsigned int)maxBr);

            uint32_t avrBr = bigEndian(pos, 4); // avg bitrate
            pos += 4;
            AUDIO_INFO("avg bitrate: %lu", (long unsigned int)avrBr);

            if ( *pos == 0x05) {;} // log_w("Found  DecoderSpecificInfo Tag (Tag: 0x05)")
            pos++;
            int decoder_specific_len = parse_variant_length((pos)); (void)decoder_specific_len;

            uint16_t ASC = bigEndian(pos, 2);

            uint8_t objectType = ASC >> 11; // first 5 bits

            if     (objectType == 1) { AUDIO_INFO("AudioObjectType: AAC Main"); } // Audio Object Types
            else if(objectType == 2) { AUDIO_INFO("AudioObjectType: AAC Low Complexity"); }
            else if(objectType == 3) { AUDIO_INFO("AudioObjectType: AAC Scalable Sample Rate"); }
            else if(objectType == 4) { AUDIO_INFO("AudioObjectType: AAC Long Term Prediction"); }
            else if(objectType == 5) { AUDIO_INFO("AudioObjectType: AAC Spectral Band Replication"); }
            else if(objectType == 6) { AUDIO_INFO("AudioObjectType: AAC Scalable"); }
            else { AUDIO_INFO("unknown ObjectType %x, stop", objectType); stopSong();}
            if(objectType < 7) m_M4A_objectType = objectType;

            const uint32_t samplingFrequencies[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
            uint8_t        sRate = (ASC & 0x0600) >> 7; // next 4 bits Sampling Frequencies
            AUDIO_INFO("Sampling Frequency: %lu", (long unsigned int)samplingFrequencies[sRate]);

            uint8_t chConfig = (ASC & 0x78) >> 3; // next 4 bits
            if(chConfig == 0) AUDIO_INFO("Channel Configurations: AOT Specifc Config");
            if(chConfig == 1) AUDIO_INFO("Channel Configurations: front-center");
            if(chConfig == 2) AUDIO_INFO("Channel Configurations: front-left, front-right");
            if(chConfig > 2) { log_e("Channel Configurations with more than 2 channels is not allowed, stop!"); stopSong();}
            if(chConfig < 3) m_M4A_chConfig = chConfig;

            uint8_t frameLengthFlag = (ASC & 0x04);
            uint8_t dependsOnCoreCoder = (ASC & 0x02);
            (void)dependsOnCoreCoder;
            uint8_t extensionFlag = (ASC & 0x01);
            (void)extensionFlag;

            if(frameLengthFlag == 0) AUDIO_INFO("AAC FrameLength: 1024 bytes");
            if(frameLengthFlag == 1) AUDIO_INFO("AAC FrameLength: 960 bytes");
        }
        if(specialIndexOf(data, "mp4a", len) > 0) {
            int offset = specialIndexOf(data, "mp4a", len);
            int channel = bigEndian(data + offset + 20, 2); // audio parameter must be set before starting
            int bps = bigEndian(data + offset + 22, 2);     // the aac decoder. There are RAW blocks only in m4a
            int srate = bigEndian(data + offset + 26, 4);   //
            setBitsPerSample(bps);
            setChannels(channel);
            if(!m_M4A_chConfig) m_M4A_chConfig = channel;
            setSampleRate(srate);
            m_M4A_sampleRate = srate;
            setBitrate(bps * channel * srate);
            AUDIO_INFO("ch; %i, bps: %i, sr: %i", channel, bps, srate);
            if(m4aHdr.audioDataPos && m_dataMode == AUDIO_LOCALFILE) {
                m_controlCounter = M4A_AMRDY;
                setFilePos(m4aHdr.audioDataPos);
                return 0;
            }
        }
        m_controlCounter = M4A_MOOV;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_controlCounter == M4A_ILST) { // ilst
        const char info[12][6] = {"nam\0", "ART\0", "alb\0", "too\0", "cmt\0", "wrt\0", "tmpo\0", "trkn\0", "day\0", "cpil\0", "aART\0", "gen\0"};
        int        offset = 0;
        // If it's a local file, the metadata has already been read, even if it comes after the audio block.
        // In the event that they are in front of the audio block in a web stream, read them now
        if(!m_f_m4aID3dataAreRead) {
            ps_ptr<char>id3tag = {};
            for(int i = 0; i < 12; i++) {
                offset = specialIndexOf(data, info[i], len, true); // seek info[] with '\0'
                if(offset > 0) {
                    offset += 19;
                    if(*(data + offset) == 0) offset++;
                    char   value[256] = {0};
                    size_t tmp = strlen((const char*)data + offset);
                    if(tmp > 254) tmp = 254;
                    memcpy(value, (data + offset), tmp);
                    value[tmp] = '\0';
                    if(i == 0) id3tag.appendf("Title: %s", value);
                    if(i == 1) id3tag.appendf("Artist: %s", value);
                    if(i == 2) id3tag.appendf("Album: %s", value);
                    if(i == 3) id3tag.appendf("Encoder: %s", value);
                    if(i == 4) id3tag.appendf("Comment: %s", value);
                    if(i == 5) id3tag.appendf("Composer: %s", value);
                    if(i == 6) id3tag.appendf("BPM: %s", value);
                    if(i == 7) id3tag.appendf("Track Number: %s", value);
                    if(i == 8) id3tag.appendf("Year: %s", value);
                    if(i == 9) id3tag.appendf("Compile: %s", value);
                    if(i == 10)id3tag.appendf("Album Artist: %s", value);
                    if(i == 11)id3tag.appendf("Types of: %s", value);
                    if(id3tag.valid()) {
                        if(audio_id3data) audio_id3data(id3tag.get());
                    }
                }
            }
        }
        offset = specialIndexOf(data, "covr", len);
        if(offset > 0){
            m4aHdr.picLen = bigEndian(data + offset + 4, 4) - 4;
            m4aHdr.picPos = m4aHdr.headerSize + offset + 12;
        }
        m_controlCounter = M4A_MOOV;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    uint8_t extLen = 0;
    if(m_controlCounter == M4A_MDAT) {            // mdat
        m_audioDataSize = bigEndian(data, 4); // length of this atom

        // Extended Size
        // 00 00 00 01 6D 64 61 74 00 00 00 00 00 00 16 64
        //        0001  m  d  a  t                    5732

        if(m_audioDataSize == 1){ // Extended Size
            m_audioDataSize = bigEndian(data + 8, 8);
            m_audioDataSize -= 16;
            extLen = 8;
        }
        else m_audioDataSize -= 8;
        AUDIO_INFO("Audio-Length: %i", m_audioDataSize);
        m4aHdr.retvalue = 8 + extLen;
        m4aHdr.headerSize += 8 + extLen;
        m_controlCounter = M4A_AMRDY; // last step before starting the audio
        return 0;
    }

    if(m_controlCounter == M4A_AMRDY) { // almost ready
        m_audioDataStart = m4aHdr.headerSize;
        if(m_dataMode == AUDIO_LOCALFILE) {
            m_contentlength = m4aHdr.headerSize + m_audioDataSize; // after this mdat atom there may be other atoms
            if(extLen) m_contentlength -= 16;
            AUDIO_INFO("Content-Length: %lu", (long unsigned int)m_contentlength);
        }
#ifndef AUDIO_NO_SD_FS
        if(m4aHdr.picLen) {
            size_t pos = m_audiofile.position();
            audio_id3image(m_audiofile, m4aHdr.picPos, m4aHdr.picLen);
            m_audiofile.seek(pos); // the filepointer could have been changed by the user, set it back
        }
#endif
        m_controlCounter = M4A_OKAY; // that's all
        return 0;
    }
    // this section should never be reached
    log_e("error");
    return 0;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
size_t Audio::process_m3u8_ID3_Header(uint8_t* packet) {
    uint8_t  ID3version;
    size_t   id3Size;
    bool     m_f_unsync = false, m_f_exthdr = false;
    uint64_t current_timestamp = 0;

    (void)m_f_unsync;        // suppress -Wunused-variable
    (void)current_timestamp; // suppress -Wunused-variable

    if(specialIndexOf(packet, "ID3", 4) != 0) { // ID3 not found
        // log_i("m3u8 file has no mp3 tag");
        return 0; // error, no ID3 signature found
    }
    ID3version = *(packet + 3);
    switch(ID3version) {
        case 2:
            m_f_unsync = (*(packet + 5) & 0x80);
            m_f_exthdr = false;
            break;
        case 3:
        case 4:
            m_f_unsync = (*(packet + 5) & 0x80); // bit7
            m_f_exthdr = (*(packet + 5) & 0x40); // bit6 extended header
            break;
    };
    id3Size = bigEndian(&packet[6], 4, 7); //  ID3v2 size  4 * %0xxxxxxx (shift left seven times!!)
    id3Size += 10;
    // log_i("ID3 framesSize: %i", id3Size);
    // log_i("ID3 version: 2.%i", ID3version);

    if(m_f_exthdr) {
        log_e("ID3 extended header in m3u8 files not supported");
        return 0;
    }
    // log_i("ID3 normal frames");

    if(specialIndexOf(&packet[10], "PRIV", 5) != 0) { // tag PRIV not found
        log_e("tag PRIV in m3u8 Id3 Header not found");
        return 0;
    }
    // if tag PRIV exists assume content is "com.apple.streaming.transportStreamTimestamp"
    // a time stamp is expected in the header.

    current_timestamp = (double)bigEndian(&packet[69], 4) / 90000; // seconds

    return id3Size;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::stopSong() {
    m_f_lockInBuffer = true; // wait for the decoding to finish
        uint8_t maxWait = 0;
        while(m_f_audioTaskIsDecoding) {vTaskDelay(1); maxWait++; if(maxWait > 100) break;} // in case of error wait max 100ms
        uint32_t pos = 0;
        if(m_f_running) {
            m_f_running = false;
            if(m_dataMode == AUDIO_LOCALFILE) {
                pos = getFilePos() - inBufferFilled();
            }
            if(_client->connected()) _client->stop();
        }
#ifndef AUDIO_NO_SD_FS
        if(m_audiofile) {
            // added this before putting 'm_f_localfile = false' in stopSong(); shoulf never occur....
            AUDIO_INFO("Closing audio file \"%s\"", m_audiofile.name());
            m_audiofile.close();
        }
#endif
        memset(m_filterBuff, 0, sizeof(m_filterBuff)); // Clear FilterBuffer
        if(m_codec == CODEC_MP3) MP3Decoder_FreeBuffers();
        if(m_codec == CODEC_AAC) AACDecoder_FreeBuffers();
        if(m_codec == CODEC_M4A) AACDecoder_FreeBuffers();
        if(m_codec == CODEC_FLAC) FLACDecoder_FreeBuffers();
        if(m_codec == CODEC_OPUS) OPUSDecoder_FreeBuffers();
        if(m_codec == CODEC_VORBIS) VORBISDecoder_FreeBuffers();
        m_validSamples = 0;
        m_audioCurrentTime = 0;
        m_audioFileDuration = 0;
        m_codec = CODEC_NONE;
        m_dataMode = AUDIO_NONE;
        m_streamType = ST_NONE;
        m_playlistFormat = FORMAT_NONE;
        m_f_lockInBuffer = false;
    return pos;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::pauseResume() {
    xSemaphoreTake(mutex_audioTask, 0.3 * configTICK_RATE_HZ);
    bool retVal = false;
    if(m_dataMode == AUDIO_LOCALFILE || m_streamType == ST_WEBSTREAM || m_streamType == ST_WEBFILE) {
        m_f_running = !m_f_running;
        retVal = true;
        if(!m_f_running) {
            memset(m_outBuff.get(), 0, m_outbuffSize * sizeof(int16_t)); // Clear OutputBuffer
            memset(m_samplesBuff48K.get(), 0, m_samplesBuff48KSize * sizeof(int16_t)); // Clear SamplesBuffer
            m_validSamples = 0;
        }
    }
    xSemaphoreGive(mutex_audioTask);
    return retVal;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
size_t Audio::resampleTo48kStereo(const int16_t* input, size_t inputSamples) {

    float ratio = static_cast<float>(m_sampleRate) / 48000.0f;
    float cursor = m_resampleCursor;

    // Anzahl Input-Samples + 3 History-Samples (vorherige 3 Stereo-Frames)
    size_t extendedSamples = inputSamples + 3;

    // Temporärer Buffer: History + aktueller Input
    std::vector<int16_t> extendedInput(extendedSamples * 2);

    // Historie an den Anfang kopieren (6 Werte = 3 Stereo-Samples)
    memcpy(&extendedInput[0], m_inputHistory, 6 * sizeof(int16_t));

    // Aktuelles Input danach einfügen
    memcpy(&extendedInput[6], input, inputSamples * 2 * sizeof(int16_t));

    size_t outputIndex = 0;

    auto clipToInt16 = [](float value) -> int16_t {
        if (value > 32767.0f){log_e("overflow +"); return 32767;}
        if (value < -32768.0f) {log_e("overflow -");return -32768;}
        return static_cast<int16_t>(value);
    };

    for (size_t inIdx = 1; inIdx < extendedSamples - 2; ++inIdx) {
        int32_t xm1_l = clipToInt16(extendedInput[(inIdx - 1) * 2]);
        int32_t x0_l  = clipToInt16(extendedInput[(inIdx + 0) * 2]);
        int32_t x1_l  = clipToInt16(extendedInput[(inIdx + 1) * 2]);
        int32_t x2_l  = clipToInt16(extendedInput[(inIdx + 2) * 2]);

        int32_t xm1_r = clipToInt16(extendedInput[(inIdx - 1) * 2 + 1]);
        int32_t x0_r  = clipToInt16(extendedInput[(inIdx + 0) * 2 + 1]);
        int32_t x1_r  = clipToInt16(extendedInput[(inIdx + 1) * 2 + 1]);
        int32_t x2_r  = clipToInt16(extendedInput[(inIdx + 2) * 2 + 1]);

        while (cursor < 1.0f) {
            float t = cursor;

            // Catmull-Rom spline, construct a cubic curve
            auto catmullRom = [](float t, float xm1, float x0, float x1, float x2) {
                return 0.5f * (
                    (2.0f * x0) +
                    (-xm1 + x1) * t +
                    (2.0f * xm1 - 5.0f * x0 + 4.0f * x1 - x2) * t * t +
                    (-xm1 + 3.0f * x0 - 3.0f * x1 + x2) * t * t * t
                );
            };

            int16_t outLeft = static_cast<int16_t>(
                catmullRom(t, xm1_l, x0_l, x1_l, x2_l)
            );
            int16_t outRight = static_cast<int16_t>(
                catmullRom(t, xm1_r, x0_r, x1_r, x2_r)
            );

            m_samplesBuff48K[outputIndex * 2]     = clipToInt16(outLeft);
            m_samplesBuff48K[outputIndex * 2 + 1] = clipToInt16(outRight);

            ++outputIndex;
            cursor += ratio;
        }

        cursor -= 1.0f;
    }

    // Letzte 3 Stereo-Samples als neue Historie sichern
    for (int i = 0; i < 3; ++i) {
        size_t idx = inputSamples - 3 + i;
        m_inputHistory[i * 2]     = input[idx * 2];
        m_inputHistory[i * 2 + 1] = input[idx * 2 + 1];
    }
    m_resampleCursor = cursor;
    return outputIndex;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void IRAM_ATTR Audio::playChunk() {
    if(m_validSamples == 0) return; // nothing to do

    plCh.validSamples = 0;
    plCh.i2s_bytesConsumed = 0;
    plCh.sample[0] = 0;
    plCh.sample[1] = 0;
    plCh.s2 = 0;
    plCh.sampleSize = 4; // 2 bytes per sample (int16_t) * 2 channels
    plCh.err = ESP_OK;
    plCh.i = 0;

    if(plCh.count > 0) goto i2swrite;

    if(getChannels() == 1){
        for (int i = m_validSamples - 1; i >= 0; --i) {
            int16_t sample = m_outBuff[i];
            m_outBuff[2 * i] = sample;
            m_outBuff[2 * i + 1] = sample;
        }
    //    m_validSamples *= 2;
    }

    plCh.validSamples = m_validSamples;

    while(plCh.validSamples) {
        *plCh.sample = m_outBuff.get() + plCh.i;
        computeVUlevel(*plCh.sample);

        //---------- Filterchain, can commented out if not used-------------
        {
            if(m_corr > 1) {
                plCh.s2 = *plCh.sample;
                plCh.s2[LEFTCHANNEL] /= m_corr;
                plCh.s2[RIGHTCHANNEL] /= m_corr;
            }
            IIR_filterChain0(*plCh.sample);
            IIR_filterChain1(*plCh.sample);
            IIR_filterChain2(*plCh.sample);
        }
        //------------------------------------------------------------------
        if(m_f_forceMono && m_channels == 2){
            int32_t xy = ((*plCh.sample)[RIGHTCHANNEL] + (*plCh.sample)[LEFTCHANNEL]) / 2;
            (*plCh.sample)[RIGHTCHANNEL] = (int16_t)xy;
            (*plCh.sample)[LEFTCHANNEL]  = (int16_t)xy;
        }
        Gain(*plCh.sample);
        plCh.i += 2;
        plCh.validSamples -= 1;
    }
    //------------------------------------------------------------------------------------------
    plCh.samples48K = resampleTo48kStereo(m_outBuff.get(), m_validSamples);

    if(audio_process_i2s) {
        // processing the audio samples from external before forwarding them to i2s
        bool continueI2S = false;
        audio_process_i2s((int16_t*)m_samplesBuff48K.get(), plCh.samples48K, &continueI2S); // 48KHz stereo 16bps
        if(!continueI2S) {
            plCh.samples48K = 0;
            plCh.count = 0;
            return;
        }
    }

i2swrite:

    plCh.err = i2s_channel_write(m_i2s_tx_handle, (int16_t*)m_samplesBuff48K.get() + plCh.count, plCh.samples48K * plCh.sampleSize, &plCh.i2s_bytesConsumed, 50);
    if( ! (plCh.err == ESP_OK || plCh.err == ESP_ERR_TIMEOUT)) goto exit;
    plCh.samples48K -= plCh.i2s_bytesConsumed / plCh.sampleSize;
    plCh.count += plCh.i2s_bytesConsumed / 2;
    if(plCh.samples48K <= 0) { m_validSamples = 0; plCh.count = 0; }

// ---- statistics, bytes written to I2S (every 10s)
    // static int cnt = 0;
    // static uint32_t t = millis();

    // if(t + 10000 < millis()){
    //     log_w("%i", cnt);
    //     cnt = 0;
    //     t = millis();
    // }
    // cnt+= i2s_bytesConsumed;
//-------------------------------------------


    return;
exit:
    if     (plCh.err == ESP_OK) return;
    else if(plCh.err == ESP_ERR_INVALID_ARG)   log_e("NULL pointer or this handle is not tx handle");
    else if(plCh.err == ESP_ERR_TIMEOUT)       log_e("Writing timeout, no writing event received from ISR within ticks_to_wait");
    else if(plCh.err == ESP_ERR_INVALID_STATE) log_e("I2S is not ready to write");
    else log_e("i2s err %i", plCh.err);
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::loop() {
    if(!m_f_running) return;

    if(m_playlistFormat != FORMAT_M3U8) { // normal process
        switch(m_dataMode) {
#ifndef AUDIO_NO_SD_FS	
            case AUDIO_LOCALFILE:
                processLocalFile(); break;
#endif  // AUDIO_NO_SD_FS                
            case HTTP_RESPONSE_HEADER:
                if(!parseHttpResponseHeader()) {
                    if(m_f_timeout && lVar.count < 3) {m_f_timeout = false; lVar.count++; connecttohost(m_lastHost.get());}
                }
                else{
                    lVar.count = 0;
                }
                break;
            case AUDIO_PLAYLISTINIT: readPlayListData(); break;
            case AUDIO_PLAYLISTDATA:
                if(m_playlistFormat == FORMAT_M3U) connecttohost(parsePlaylist_M3U());
                if(m_playlistFormat == FORMAT_PLS) connecttohost(parsePlaylist_PLS());
                if(m_playlistFormat == FORMAT_ASX) connecttohost(parsePlaylist_ASX());
                break;
            case AUDIO_DATA:
                if(m_streamType == ST_WEBSTREAM) processWebStream();
                if(m_streamType == ST_WEBFILE) processWebFile();
                break;
        }
    }
    else { // m3u8 datastream only
        ps_ptr<char> host;
        if(lVar.no_host_timer > millis()) {return;}
        switch(m_dataMode) {
            case HTTP_RESPONSE_HEADER:
                if(!parseHttpResponseHeader()) {
                    if(m_f_timeout && lVar.count < 3) {m_f_timeout = false; lVar.count++; m_f_reset_m3u8Codec = false; connecttohost(m_lastHost.get());}
                }
                else{
                    lVar.count = 0;
                    m_f_firstCall  = true;
                }
                break;
            case AUDIO_PLAYLISTINIT: readPlayListData(); break;
            case AUDIO_PLAYLISTDATA:
                host = parsePlaylist_M3U8();
                if(!host.valid()) lVar.no_host_cnt++;
                else {lVar.no_host_cnt = 0; lVar.no_host_timer = millis();}

                if(lVar.no_host_cnt == 2){lVar.no_host_timer = millis() + 2000;} // no new url? wait 2 seconds
                if(host.valid()) { // host contains the next playlist URL
                    httpPrint(host.get());
                    m_dataMode = HTTP_RESPONSE_HEADER;
                }
                else { // host == NULL means connect to m3u8 URL
                    if(m_lastM3U8host.valid()) {m_f_reset_m3u8Codec = false; connecttohost(m_lastM3U8host.get());}
                    else                       {httpPrint(m_lastHost.get());}      // if url has no first redirection
                    m_dataMode = HTTP_RESPONSE_HEADER;               // we have a new playlist now
                }
                break;
            case AUDIO_DATA:
                if(m_f_ts) { processWebStreamTS(); } // aac or aacp with ts packets
                else { processWebStreamHLS(); }      // aac or aacp normal stream

                if(m_f_continue) { // at this point m_f_continue is true, means processWebStream() needs more data
                    m_dataMode = AUDIO_PLAYLISTDATA;
                    m_f_continue = false;
                }
                break;
        }
    }
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::readPlayListData() {
    if(m_dataMode != AUDIO_PLAYLISTINIT) return false;
    if(_client->available() == 0) return false;

    uint32_t chunksize = 0;
    uint8_t  readedBytes = 0;
    if(m_f_chunked) chunksize = readChunkSize(&readedBytes);

    // reads the content of the playlist and stores it in the vector m_contentlength
    // m_contentlength is a table of pointers to the lines
    ps_ptr<char>pl;
    uint32_t ctl = 0;
    int      lines = 0;
    // delete all memory in m_playlistContent
    if(m_playlistFormat == FORMAT_M3U8 && !psramFound()) { log_e("m3u8 playlists requires PSRAM enabled!"); }
    vector_clear_and_shrink(m_playlistContent);
    while(true) { // outer while

        uint32_t ctime = millis();
        uint32_t timeout = 2000; // ms
        pl.alloc(1024,"pl"); pl.clear(); // playlistLine

        while(true) { // inner while
            uint16_t pos = 0;
            while(_client->available()) { // super inner while :-))
                pl[pos] = _client->read();
                ctl++;
                if(pl[pos] == '\n') {
                    pl[pos] = '\0';
                    pos++;
                    break;
                }
                //    if(pl[pos] == '&' ) {pl[pos] = '\0'; pos++; break;}
                if(pl[pos] == '\r') {
                    pl[pos] = '\0';
                    pos++;
                    continue;
                    ;
                }
                pos++;
                if(pos == 1022) {
                    pos--;
                    continue;
                }
                if(pos == 509) { pl[pos] = '\0'; }
                if(ctl == chunksize) {
                    pl[pos] = '\0';
                    break;
                }
                if(ctl == m_contentlength) {
                    pl[pos] = '\0';
                    break;
                }
            }
            if(ctl == chunksize) break;
            if(ctl == m_contentlength) break;
            if(pos) {
                pl[pos] = '\0';
                break;
            }

            if(ctime + timeout < millis()) {
                log_e("timeout");
                for(int i = 0; i < m_playlistContent.size(); i++) log_e("pl%i = %s", i, m_playlistContent[i].get());
                goto exit;
            }
        } // inner while

        if(pl.starts_with_icase("<!DOCTYPE")) {
            AUDIO_INFO("url is a webpage!");
            goto exit;
        }
        if(pl.starts_with_icase("<html")) {
            AUDIO_INFO("url is a webpage!");
            goto exit;
        }
        if(pl.size() > 0) m_playlistContent.emplace_back(std::move(pl));

        if(m_playlistContent.size() && m_playlistContent.size() % 1000 == 0) { AUDIO_INFO("current playlist line: %lu", (long unsigned)m_playlistContent.size()); }
        // termination conditions
        // 1. The http response header returns a value for contentLength -> read chars until contentLength is reached
        // 2. no contentLength, but Transfer-Encoding:chunked -> compute chunksize and read until chunksize is reached
        // 3. no chunksize and no contentlengt, but Connection: close -> read all available chars
        if(ctl == m_contentlength) {
            while(_client->available()) _client->read();
            break;
        } // read '\n\n' if exists
        if(ctl == chunksize) {
            while(_client->available()) _client->read();
            break;
        }
        if(!_client->connected() && _client->available() == 0) break;

    } // outer while
    lines = m_playlistContent.size();
    for(int i = 0; i < lines; i++) { // print all string in first vector of 'arr'
    //    log_w("pl=%i \"%s\"", i, m_playlistContent[i]);
    }
    m_dataMode = AUDIO_PLAYLISTDATA;
    return true;

exit:
    vector_clear_and_shrink(m_playlistContent);
    m_f_running = false;
    m_dataMode = AUDIO_NONE;
    return false;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const char* Audio::parsePlaylist_M3U() {
    uint8_t lines = m_playlistContent.size();
    int     pos = 0;
    char*   host = nullptr;

    for(int i = 0; i < lines; i++) {
        if(m_playlistContent[i].contains("#EXTINF:")) { // Info?
            pos = m_playlistContent[i].index_of(",");        // Comma in this line?
            if(pos > 0) {
                // Show artist and title if present in metadata
                AUDIO_INFO(m_playlistContent[i].get() + pos + 1);
            }
            continue;
        }
        if(m_playlistContent[i].starts_with("#")) { // Commentline?
            continue;
        }

        pos = m_playlistContent[i].index_of("http://:@", 0); // ":@"??  remove that!
        if(pos >= 0) {
            AUDIO_INFO("Entry in playlist found: %s", (m_playlistContent[i].get() + pos + 9));
            host = m_playlistContent[i].get() + pos + 9;
            break;
        }
        // AUDIO_INFO("Entry in playlist found: %s", pl);
        pos = m_playlistContent[i].index_of("http", 0); // Search for "http"
        if(pos >= 0) {                                  // Does URL contain "http://"?
                                                        //    log_e("%s pos=%i", m_playlistContent[i], pos);
            host = m_playlistContent[i].get() + pos;          // Yes, set new host
            break;
        }
    }
    //    vector_clear_and_shrink(m_playlistContent);
    return host;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const char* Audio::parsePlaylist_PLS() {
    uint8_t lines = m_playlistContent.size();
    int     pos = 0;
    char*   host = nullptr;

    for(int i = 0; i < lines; i++) {
        if(i == 0) {
            if(m_playlistContent[0].strlen() == 0) goto exit;     // empty line
            if(!m_playlistContent[0].starts_with("[playlist]")) { // first entry in valid pls
                m_dataMode = HTTP_RESPONSE_HEADER;                // pls is not valid
                AUDIO_INFO("pls is not valid, switch to HTTP_RESPONSE_HEADER");
                goto exit;
            }
            continue;
        }
        if(m_playlistContent[i].starts_with("File1")) {
            if(host) continue;                              // we have already a url
            pos = m_playlistContent[i].index_of("http", 0); // File1=http://streamplus30.leonex.de:14840/;
            if(pos >= 0) {                                  // yes, URL contains "http"?
                host = m_playlistContent[i].get() + pos;          // Now we have an URL for a stream in host.
            }
            continue;
        }
        if(m_playlistContent[i].starts_with("Title1")) { // Title1=Antenne Tirol
            const char* plsStationName = (m_playlistContent[i].get() + 7);
            if(audio_showstation) audio_showstation(plsStationName);
            AUDIO_INFO("StationName: \"%s\"", plsStationName);
            continue;
        }
        if(m_playlistContent[i].starts_with("Length1")) { continue; }
        if(m_playlistContent[i].contains("Invalid username")) { // Unable to access account:
            goto exit;                                          // Invalid username or password
        }
    }
    return host;

exit:
    m_f_running = false;
    stopSong();
    vector_clear_and_shrink(m_playlistContent);
    m_dataMode = AUDIO_NONE;
    return nullptr;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const char* Audio::parsePlaylist_ASX() { // Advanced Stream Redirector
    uint8_t lines = m_playlistContent.size();
    bool    f_entry = false;
    int     pos = 0;
    char*   host = nullptr;

    for(int i = 0; i < lines; i++) {
        int p1 = m_playlistContent[i].index_of("<", 0);
        int p2 = m_playlistContent[i].index_of(">", 1);
        if(p1 >= 0 && p2 > p1) { // #196 set all between "< ...> to lowercase
            for(uint8_t j = p1; j < p2; j++) { m_playlistContent[i][j] = toLowerCase(m_playlistContent[i][j]); }
        }
        if(m_playlistContent[i].contains("<entry>")) f_entry = true; // found entry tag (returns -1 if not found)
        if(f_entry) {
            if(m_playlistContent[i].contains("ref href")) { //  <ref href="http://87.98.217.63:24112/stream" />
                pos = m_playlistContent[i].index_of("http", 0);
                if(pos > 0) {
                    host = (m_playlistContent[i].get() + pos); // http://87.98.217.63:24112/stream" />
                    int pos1 = indexOf(host, "\"", 0);   // http://87.98.217.63:24112/stream
                    if(pos1 > 0) host[pos1] = '\0';      // Now we have an URL for a stream in host.
                }
            }
        }
        pos = m_playlistContent[i].index_of("<title>", 0);
        if(pos >= 0) {
            char* plsStationName = (m_playlistContent[i].get() + pos + 7); // remove <Title>
            pos = indexOf(plsStationName, "</", 0);
            if(pos >= 0) {
                *(plsStationName + pos) = 0; // remove </Title>
            }
            if(audio_showstation) audio_showstation(plsStationName);
            AUDIO_INFO("StationName: \"%s\"", plsStationName);
        }

        if(m_playlistContent[i].starts_with("http") && !f_entry) { // url only in asx
            host = m_playlistContent[i].get();
        }
    }
    return host;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
ps_ptr<char> Audio::parsePlaylist_M3U8() {
    // example: audio chunks
    // #EXTM3U
    // #EXT-X-TARGETDURATION:10
    // #EXT-X-MEDIA-SEQUENCE:163374040
    // #EXT-X-DISCONTINUITY
    // #EXTINF:10,title="text=\"Spot Block End\" amgTrackId=\"9876543\"",artist=" ",url="length=\"00:00:00\""
    // http://n3fa-e2.revma.ihrhls.com/zc7729/63_sdtszizjcjbz02/main/163374038.aac
    // #EXTINF:10,title="text=\"Spot Block End\" amgTrackId=\"9876543\"",artist=" ",url="length=\"00:00:00\""
    // http://n3fa-e2.revma.ihrhls.com/zc7729/63_sdtszizjcjbz02/main/163374039.aac

    if(!m_lastHost.valid())      {log_e("m_lastHost is NULL");     return {};} // guard

    uint8_t lines = m_playlistContent.size();
    bool    f_haveRedirection = false;
    char    llasc[21]; // uint64_t max = 18,446,744,073,709,551,615  thats 20 chars + \0

    if(lines){
        bool addNextLine = false;
        vector_clear_and_shrink(m_linesWithSeqNrAndURL);
        vector_clear_and_shrink(m_linesWithEXTINF);
        for(uint8_t i = 0; i < lines; i++) {
            //  log_w("pl%i = %s", i, m_playlistContent[i].get());
            if(m_playlistContent[i].starts_with("#EXT-X-STREAM-INF:")) { f_haveRedirection = true; /*log_e("we have a redirection");*/}
            if(addNextLine) {
                addNextLine = false;
                // size_t len = strlen(linesWithSeqNr[idx].get()) + strlen(m_playlistContent[i].get()) + 1;
                m_linesWithSeqNrAndURL.emplace_back().clone_from(m_playlistContent[i]);
            }
            if(startsWith(m_playlistContent[i].get(), "#EXTINF:")) {
                m_linesWithEXTINF.emplace_back().clone_from(m_playlistContent[i]);
                addNextLine = true;
            }
        }
    }

    for(int i = 0; i < m_linesWithSeqNrAndURL.size(); i++) {/*log_w("%s", m_linesWithSeqNrAndURL[i].get())*/;}
    for(int i = 0; i < m_linesWithEXTINF.size(); i++)      {/*log_w("%s", m_linesWithEXTINF[i].get());*/ showstreamtitle(m_linesWithEXTINF[i].get());}

    if(f_haveRedirection) {
        ps_ptr<char> ret = m3u8redirection(&m_m3u8Codec);
        m_lastM3U8host.assign(ret.get());
        vector_clear_and_shrink(m_playlistContent);
        return {};
    }

    if(m_f_firstM3U8call) {
        m_f_firstM3U8call = false;
        pplM3U8.xMedSeq = 0;
        pplM3U8.f_mediaSeq_found = false;
    }

    if(!pplM3U8.f_mediaSeq_found) {pplM3U8.f_mediaSeq_found = m3u8_findMediaSeqInURL(m_linesWithSeqNrAndURL, &pplM3U8.xMedSeq); AUDIO_INFO("MediaSequenceNumber: %lli", pplM3U8.xMedSeq);}
    if(m_codec == CODEC_NONE) {m_codec = CODEC_AAC; if(m_m3u8Codec == CODEC_MP3) m_codec = CODEC_MP3;}  // if we have no redirection
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

    boolean f_EXTINF_found = false;

    if(lines) {
        for(uint16_t i = 0; i < m_linesWithSeqNrAndURL.size(); i++) {

            f_EXTINF_found = true;

            ps_ptr<char> tmp;
            if(!m_linesWithSeqNrAndURL[i].starts_with("http")) {

                //  playlist:   http://station.com/aaa/bbb/xxx.m3u8
                //  chunklist:  http://station.com/aaa/bbb/ddd.aac
                //  result:     http://station.com/aaa/bbb/ddd.aac

                if(m_lastM3U8host.valid()) {
                    tmp.clone_from(m_lastM3U8host, "tmp");
                }
                else {
                    tmp.clone_from(m_lastHost, "tmp");
                }
                if(m_linesWithSeqNrAndURL[i][0] != '/'){

                    //  playlist:   http://station.com/aaa/bbb/xxx.m3u8  // tmp
                    //  chunklist:  ddd.aac                              // m_linesWithSeqNrAndURL[i]
                    //  result:     http://station.com/aaa/bbb/ddd.aac   // m_linesWithSeqNrAndURL[i]

                    int idx = tmp.last_index_of('/');
                    tmp[idx  + 1] = '\0';
                    tmp.append(m_linesWithSeqNrAndURL[i].get());
                }
                else{

                    //  playlist:   http://station.com/aaa/bbb/xxx.m3u8
                    //  chunklist:  /aaa/bbb/ddd.aac
                    //  result:     http://station.com/aaa/bbb/ddd.aac
                    int idx = tmp.index_of('/', 8);
                    tmp[idx] = '\0';
                    tmp.append(m_linesWithSeqNrAndURL[i].get());
                }
            }
            else {tmp.append(m_linesWithSeqNrAndURL[i].get());}

            if(pplM3U8.f_mediaSeq_found) {
                lltoa(pplM3U8.xMedSeq, llasc, 10);
                if(tmp.contains(llasc)) {
                    m_playlistURL.insert(m_playlistURL.begin(), std:: move(tmp));
                    pplM3U8.xMedSeq++;
                }
                else{
                    lltoa(pplM3U8.xMedSeq + 1, llasc, 10);
                    if(tmp.contains(llasc)) {
                        m_playlistURL.insert(m_playlistURL.begin(), std::move(tmp));
                        log_w("mediaseq %llu skipped", pplM3U8.xMedSeq);
                        pplM3U8.xMedSeq+= 2;
                    }
                }
            }
            else { // without mediaSeqNr, with hash
                uint32_t hash = simpleHash(tmp.get());
                if(m_hashQueue.size() == 0) {
                    m_hashQueue.insert(m_hashQueue.begin(), hash);
                    m_playlistURL.insert(m_playlistURL.begin(), std::move(tmp));
                }
                else {
                    bool known = false;
                    for(int i = 0; i < m_hashQueue.size(); i++) {
                        if(hash == m_hashQueue[i]) {
                            // log_i("file already known %s", tmp);
                            known = true;
                        }
                    }
                    if(!known) {
                        m_hashQueue.insert(m_hashQueue.begin(), hash);
                        m_playlistURL.insert(m_playlistURL.begin(), std::move(tmp));
                    }
                }
                if(m_hashQueue.size() > 20) m_hashQueue.pop_back();
            }
            continue;
        }
        vector_clear_and_shrink(m_playlistContent); // clear after reading everything, m_playlistContent.size is now 0
    }
    if(m_playlistURL.size() > 0) {
        ps_ptr<char>m_playlistBuff;
        if(m_playlistURL[m_playlistURL.size() - 1].valid()) {
            m_playlistBuff.append(m_playlistURL[m_playlistURL.size() - 1].get());
            m_playlistURL.pop_back();
            m_playlistURL.shrink_to_fit();
        }
        // log_i("now playing %s", m_playlistBuff);
        if(endsWith(m_playlistBuff.get(), "ts")) m_f_ts = true;
        if(m_playlistBuff.contains(".ts?") > 0)  m_f_ts = true;
        return m_playlistBuff;
    }
    else {
        if(f_EXTINF_found) {
            if(pplM3U8.f_mediaSeq_found) {
                if(m_playlistContent.size() == 0) return {};

                uint64_t mediaSeq;
                pplM3U8.f_mediaSeq_found = m3u8_findMediaSeqInURL(m_linesWithSeqNrAndURL, &mediaSeq);
                if(!pplM3U8.f_mediaSeq_found) {
                    log_e("xMediaSequence not found");
                    connecttohost(m_lastHost.get());
                }
                if(mediaSeq < pplM3U8.xMedSeq) {
                    uint64_t diff = pplM3U8.xMedSeq - mediaSeq;
                    if(diff < 10) { ; }
                    else {
                        if(m_playlistContent.size() > 0) {
                            for(int j = 0; j < lines; j++) {
                                // log_i("lines %i, %s", lines, m_playlistContent[j]);
                            }
                        }
                        else { ; }

                        if(m_playlistURL.size() > 0) {
                            for(int j = 0; j < m_playlistURL.size(); j++) {
                                // log_i("m_playlistURL lines %i, %s", j, m_playlistURL[j]);
                            }
                        }
                        else { ; }

                        if(m_playlistURL.size() == 0) {
                            m_f_reset_m3u8Codec = false;
                            connecttohost(m_lastHost.get());
                        }
                    }
                }
                else {
                    if(mediaSeq != UINT64_MAX) { log_e("err, %u packets lost from %u, to %u", mediaSeq - pplM3U8.xMedSeq, pplM3U8.xMedSeq, mediaSeq); }
                    pplM3U8.xMedSeq = mediaSeq;
                }
            } // f_medSeq_found
        }
    }
    return {};
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

ps_ptr<char>Audio::m3u8redirection(uint8_t* codec) {
    // example: redirection
    // #EXTM3U
    // #EXT-X-STREAM-INF:BANDWIDTH=117500,AVERAGE-BANDWIDTH=117000,CODECS="mp4a.40.2"
    // 112/playlist.m3u8?hlssid=7562d0e101b84aeea0fa35f8b963a174
    // #EXT-X-STREAM-INF:BANDWIDTH=69500,AVERAGE-BANDWIDTH=69000,CODECS="mp4a.40.5"
    // 64/playlist.m3u8?hlssid=7562d0e101b84aeea0fa35f8b963a174
    // #EXT-X-STREAM-INF:BANDWIDTH=37500,AVERAGE-BANDWIDTH=37000,CODECS="mp4a.40.29"
    // 32/playlist.m3u8?hlssid=7562d0e101b84aeea0fa35f8b963a174

    if(!m_lastHost.valid()) {log_e("m_lastHost is empty"); return {};} // guard
    const char codecString[9][11]={
        "mp4a.40.34", // mp3 stream
        "mp4a.40.01", // AAC Main
        "mp4a.40.2",  // MPEG-4 AAC LC
        "mp4a.40.02", // MPEG-4 AAC LC, leading 0 for Aud-OTI compatibility
        "mp4a.40.29", // MPEG-4 HE-AAC v2 (AAC LC + SBR + PS)
        "mp4a.40.42", // xHE-AAC
        "mp4a.40.5",  // MPEG-4 HE-AAC v1 (AAC LC + SBR)
        "mp4a.40.05", // MPEG-4 HE-AAC v1 (AAC LC + SBR), leading 0 for Aud-OTI compatibility
        "mp4a.67",    // MPEG-2 AAC LC
    };

    uint16_t choosenLine = 0;
    uint16_t plcSize = m_playlistContent.size();
    int8_t   cS = 100;

    for(uint16_t i = 0; i < plcSize; i++) { // looking for lowest codeString
        if(m_playlistContent[i].contains("CODECS=\"mp4a")){
            for (uint8_t j = 0; j < 9; j++) {
                if (m_playlistContent[i].contains(codecString[j])) {
                    if (j < cS) {
                        cS = j;
                        choosenLine = i;
                    }
                }
            }
        }
    }
    if (cS == 0)            *codec = CODEC_MP3;
    else if (cS < 100)      *codec = CODEC_AAC;



    if(cS == 100) {                             // "mp4a.xx.xx" not found
        *codec = CODEC_AAC;                     // assume AAC
        for(uint16_t i = 0; i < plcSize; i++) { // we have no codeString, looking for "http"
            if(m_playlistContent[i].contains("#EXT-X-STREAM-INF")){
                choosenLine = i;
            }
        }
    }

    choosenLine++; // next line is the redirection url

    ps_ptr<char> result = {};
    ps_ptr<char>line; line.clone_from(m_playlistContent[choosenLine]);

    if(line.starts_with("../")){
        // ../../2093120-b/RISMI/stream01/streamPlaylist.m3u8
        while (line.starts_with("../")){
            line.remove_prefix("../");
        }
        result.clone_from(m_lastHost);
        int idx = result.last_index_of('/');
        if(idx > 0) result.truncate_at((size_t)idx + 1);
        result.append(line.get());
    }
    else if(!line.starts_with_icase("http")) {

        // http://arcast.com.ar:1935/radio/radionar.stream/playlist.m3u8               m_lastHost
        // chunklist_w789468022.m3u8                                                   line
        // http://arcast.com.ar:1935/radio/radionar.stream/chunklist_w789468022.m3u8   --> result

        result.clone_from(m_lastHost);
        int pos = m_lastHost.last_index_of('/');
        if(pos > 0){
            result.truncate_at((size_t)pos + 1);
            result.append(line.get());
        }
    }
    else {
        result.clone_from(line);
    }

    return result; // it's a redirection, a new m3u8 playlist
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::m3u8_findMediaSeqInURL(std::vector<ps_ptr<char>>&linesWithSeqNr, uint64_t* mediaSeqNr) { // We have no clue what the media sequence is

    // example: linesWithSeqNr
    // p:,<p:,<10.032,media_w630738364_405061.mp3
    // p:,<p:,<9.952,media_w630738364_405062.mp3
    // p:,<p:,<10.031,media_w630738364_405063.mp3

    if(linesWithSeqNr.size() < 3) return false;
    //     for (uint16_t i = 0; i < linesWithSeqNr.size(); i++) {
    //         log_w("linesWithSeqNr[%i] = %s", i, linesWithSeqNr[i].get());
    //     }

    uint16_t pos;
    std::array<std::vector<int64_t>, 3>numbers;
    for(int i = 0; i < 3; i++){
        pos = 0;
        for(pos = 0; pos < linesWithSeqNr[i].strlen(); pos++){
            if(isdigit(linesWithSeqNr[i][pos])){
                numbers[i].push_back(strtoull(linesWithSeqNr[i].get() + pos, nullptr, 10));
                // log_w("%lli", strtoull(linesWithSeqNr[i].get() + pos, nullptr, 10));
                while(pos < linesWithSeqNr[i].strlen()){ // skip to next number or line end
                    pos++;
                    if(!isdigit(linesWithSeqNr[i][pos])) break;
                }
            }
        }
    }

    // There must be three valid lines with a sequencer.Then the integers are sought for each line.For the example, these are:
    // numbers[0] 10 32 630738364 405061
    // numbers[1] 9 952 630738364 405062
    // numbers[2] 10 31 630738364 405063
    // then the absolute value is formed (-234 is also possible if there is a hyphen) and it is searched for three consecutive numbers

    uint16_t idx = 0;
    while(idx < numbers[0].size()){
        // log_w("%lli, %lli, %lli", abs(numbers[0][idx]), abs(numbers[1][idx]), abs(numbers[2][idx]));
        if(abs(numbers[0][idx]) + 1 == abs(numbers[1][idx]) && abs(numbers[1][idx]) + 1 == abs(numbers[2][idx])){
            *mediaSeqNr = abs(numbers[0][idx]);
            return true;
        }
        idx++;
    }
    return false;
}
#ifndef AUDIO_NO_SD_FS
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::processLocalFile() {
    if(!(m_audiofile && m_f_running && m_dataMode == AUDIO_LOCALFILE)) return; // guard

    static uint32_t ctime = 0;
    static uint32_t newFilePos = 0;
    static uint32_t byteCounter = 0;
    static bool     audioHeaderFound = false;
    const uint32_t  timeout = 8000;                          // ms
    const uint32_t  maxFrameSize = InBuff.getMaxBlockSize(); // every mp3/aac frame is not bigger
    uint32_t        availableBytes = 0;
    int32_t         bytesAddedToBuffer = 0;
    int32_t         offset = 0;

    if(m_f_firstCall) { // runs only one time per connection, prepare for start
        m_f_firstCall = false;
        m_f_stream = false;
        audioHeaderFound = false;
        newFilePos = 0;
        byteCounter = 0;
        ctime = millis();
        if(m_codec == CODEC_M4A) seek_m4a_stsz(); // determine the pos of atom stsz
        if(m_codec == CODEC_M4A) seek_m4a_ilst(); // looking for metadata
        m_audioDataSize = 0;
        m_audioDataStart = 0;
        m_f_allDataReceived = false;
        return;
    }

    if(m_resumeFilePos >= 0 && newFilePos == 0) { // we have a resume file position
        if(!audioHeaderFound){log_w("timeOffset not possible"); m_resumeFilePos = -1; return;}
        if(m_resumeFilePos <  (int32_t)m_audioDataStart) m_resumeFilePos = m_audioDataStart;
        if(m_resumeFilePos >= (int32_t)m_audioDataStart + m_audioDataSize) {goto exit;}

        m_f_lockInBuffer = true;                          // lock the buffer, the InBuffer must not be re-entered in playAudioData()
            while(m_f_audioTaskIsDecoding) vTaskDelay(1); // We can't reset the InBuffer while the decoding is in progress
            InBuff.resetBuffer();
        m_f_lockInBuffer = false;
        newFilePos = m_resumeFilePos;
        m_audiofile.seek(newFilePos);
        m_f_allDataReceived = false;
        byteCounter = newFilePos;
        return;
    }

    availableBytes = InBuff.writeSpace();
    bytesAddedToBuffer = m_audiofile.read(InBuff.getWritePtr(), availableBytes);
    if(bytesAddedToBuffer > 0) {byteCounter += bytesAddedToBuffer; InBuff.bytesWritten(bytesAddedToBuffer);}
    if(m_audioDataSize && byteCounter >= m_audioDataSize){if(!m_f_allDataReceived) m_f_allDataReceived = true;}
    if(!m_audioDataSize && byteCounter == m_fileSize){if(!m_f_allDataReceived) m_f_allDataReceived = true;}
    // log_e("byteCounter %u >= m_audioDataSize %u, m_f_allDataReceived % i", byteCounter, m_audioDataSize, m_f_allDataReceived);

    if(newFilePos) { // we have a new file position
        if(InBuff.bufferFilled() < InBuff.getMaxBlockSize()) return;
        if(m_codec == CODEC_OPUS || m_codec == CODEC_VORBIS) {if(InBuff.bufferFilled() < 0xFFFF) return;} // ogg frame <= 64kB
        if(m_codec == CODEC_WAV)   {while((m_resumeFilePos % 4) != 0){m_resumeFilePos++; offset++; if(m_resumeFilePos >= m_fileSize) goto exit;}}  // must divisible by four
        if(m_codec == CODEC_MP3)   {offset = mp3_correctResumeFilePos();  if(offset == -1) goto exit; MP3Decoder_ClearBuffer();}
        if(m_codec == CODEC_FLAC)  {offset = flac_correctResumeFilePos(); if(offset == -1) goto exit; FLACDecoderReset();}
        if(m_codec == CODEC_M4A)   {offset = m4a_correctResumeFilePos();  if(offset == -1) goto exit;}
        if(m_codec == CODEC_VORBIS){offset = ogg_correctResumeFilePos();  if(offset == -1) goto exit; VORBISDecoder_ClearBuffers();}
        if(m_codec == CODEC_OPUS)  {offset = ogg_correctResumeFilePos();  if(offset == -1) goto exit; OPUSDecoder_ClearBuffers();}
        m_haveNewFilePos  = newFilePos + offset - m_audioDataStart;
        m_sumBytesDecoded = newFilePos + offset - m_audioDataStart;
        newFilePos = 0;
        m_resumeFilePos = -1;
        InBuff.bytesWasRead(offset);
        byteCounter += offset;
    }

    if(!m_f_stream) {
        if(m_codec == CODEC_OGG) { // log_e("determine correct codec here");
            uint8_t codec = determineOggCodec(InBuff.getReadPtr(), maxFrameSize);
            if     (codec == CODEC_FLAC)   {initializeDecoder(codec); m_codec = CODEC_FLAC;   return;}
            else if(codec == CODEC_OPUS)   {initializeDecoder(codec); m_codec = CODEC_OPUS;   return;}
            else if(codec == CODEC_VORBIS) {initializeDecoder(codec); m_codec = CODEC_VORBIS; return;}
            else                           {stopSong(); return;}
        }
        if(m_controlCounter != 100) {
            if((millis() - ctime) > timeout) {
                log_e("audioHeader reading timeout");
                m_f_running = false;
                goto exit;
            }
            if(InBuff.bufferFilled() > maxFrameSize || (InBuff.bufferFilled() == m_fileSize) || m_f_allDataReceived) { // at least one complete frame or the file is smaller
                InBuff.bytesWasRead(readAudioHeader(InBuff.getMaxAvailableBytes()));
            }
            if(m_controlCounter == 100){
                if(m_audioDataStart > 0){ audioHeaderFound = true; }
                if(!m_audioDataSize) m_audioDataSize = m_fileSize;
                byteCounter = getFilePos();
            }
            return;
        }
        else {
            m_f_stream = true;
            AUDIO_INFO("stream ready");
        }
    }

    if(m_fileStartPos > 0){
        setFilePos(m_fileStartPos);
        m_fileStartPos = -1;
    }

    // end of file reached? - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_eof){ // m_f_eof and m_f_ID3v1TagFound will be set in playAudioData()
        if(m_f_ID3v1TagFound) readID3V1Tag();
exit:
        ps_ptr<char>afn; // audio file name
        if(m_audiofile) afn.assign(m_audiofile.name()); // store temporary the name
        stopSong();
        m_audioCurrentTime = 0;
        m_audioFileDuration = 0;
        m_resumeFilePos = -1;
        m_haveNewFilePos = 0;
        m_codec = CODEC_NONE;

        if(afn.valid()) {
            if(audio_eof_mp3) audio_eof_mp3(afn.get());
            AUDIO_INFO("End of file \"%s\"", afn.get());
        }
        return;
    }
}
#endif  // AUDIO_NO_SD_FS	
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::processWebStream() {
    if(m_dataMode != AUDIO_DATA) return; // guard

    const uint16_t  maxFrameSize = InBuff.getMaxBlockSize(); // every mp3/aac frame is not bigger
    static uint32_t chunkSize;                               // chunkcount read from stream
    static bool     f_skipCRLF;
    uint32_t        availableBytes = 0; // available from stream
    bool            f_clientIsConnected = _client->connected();

    // first call, set some values to default  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_firstCall) { // runs only ont time per connection, prepare for start
        m_f_firstCall = false;
        m_f_stream = false;
        chunkSize = 0;
        m_metacount = m_metaint;
        f_skipCRLF = false;
        m_f_allDataReceived = false;
        readMetadata(0, true); // reset all static vars
    }
    if(f_clientIsConnected) availableBytes = _client->available(); // available from stream

    // chunked data tramsfer - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_chunked && availableBytes > 0) {
        uint8_t readedBytes = 0;
        if(!chunkSize){
            if(f_skipCRLF){
                if(_client->available() < 2) { // avoid getting out of sync
                    AUDIO_INFO("webstream chunked: not enough bytes available for skipCRLF");
                    return;
                }
                int a =_client->read(); if(a != 0x0D) log_w("chunk count error, expected: 0x0D, received: 0x%02X", a); // skipCR
                int b =_client->read(); if(b != 0x0A) log_w("chunk count error, expected: 0x0A, received: 0x%02X", b); // skipLF
                f_skipCRLF = false;
            }
            if(_client->available()){
                chunkSize = readChunkSize(&readedBytes);
                if(chunkSize > 0) {
                    f_skipCRLF = true; // skip next CRLF
                }
                // log_w("chunk size: %d", chunkSize);
            }
        }
        availableBytes = min(availableBytes, chunkSize);
    }

    // we have metadata  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_metadata && availableBytes) {
        if(m_metacount == 0) {
            int metaLen = readMetadata(availableBytes);
            chunkSize -= metaLen; // reduce chunkSize by metadata length
            return;
        }
        availableBytes = min(availableBytes, m_metacount);
    }

    // if the buffer is often almost empty issue a warning - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_stream) {
        if(!m_f_allDataReceived) if(streamDetection(availableBytes)) return;
        if(!f_clientIsConnected) {if(m_f_tts && !m_f_allDataReceived) m_f_allDataReceived = true;} // connection closed (OpenAi)
    }

    // buffer fill routine - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(availableBytes) {
        availableBytes = min(availableBytes, (uint32_t)InBuff.writeSpace());
        int32_t bytesAddedToBuffer = _client->read(InBuff.getWritePtr(), availableBytes);
        if(bytesAddedToBuffer > 0) {

            if(m_f_metadata) m_metacount -= bytesAddedToBuffer;
            if(m_f_chunked) chunkSize -= bytesAddedToBuffer;
            InBuff.bytesWritten(bytesAddedToBuffer);
        }
    }

    // start audio decoding - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(InBuff.bufferFilled() > maxFrameSize && !m_f_stream) { // waiting for buffer filled
        if(m_codec == CODEC_OGG) { // log_i("determine correct codec here");
            uint8_t codec = determineOggCodec(InBuff.getReadPtr(), maxFrameSize);
            if(codec == CODEC_FLAC) {initializeDecoder(codec); m_codec = codec;}
            if(codec == CODEC_OPUS) {initializeDecoder(codec); m_codec = codec;}
            if(codec == CODEC_VORBIS) {initializeDecoder(codec); m_codec = codec;}
        }
        AUDIO_INFO("stream ready");
        m_f_stream = true;  // ready to play the audio data
    }

    if(m_f_eof) {
        AUDIO_INFO("End of webstream: \"%s\"", m_lastHost.get());
        if(audio_eof_stream) audio_eof_stream(m_lastHost.get());
        stopSong();
    }

}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::processWebFile() {
    if(!m_lastHost.valid()) {log_e("m_lastHost is empty"); return;}  // guard
    const uint32_t  maxFrameSize = InBuff.getMaxBlockSize();        // every mp3/aac frame is not bigger
    static uint32_t chunkSize;                                      // chunkcount read from stream
    static size_t   audioDataCount;                                 // counts the decoded audiodata only
    static uint32_t byteCounter;                                    // count received data
    static bool     f_waitingForPayload = false;                    // waiting for payload
    bool            f_clientIsConnected = _client;                  // if _client is Nullptr, we are not connected

    // first call, set some values to default - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_firstCall) { // runs only ont time per connection, prepare for start
        m_f_firstCall = false;
        f_waitingForPayload = true;
        m_t0 = millis();
        byteCounter = 0;
        chunkSize = 0;
        audioDataCount = 0;
        m_f_stream = false;
        m_audioDataSize = m_contentlength;
        m_webFilePos = 0;
        m_controlCounter = 0;
        m_f_allDataReceived = false;
    }

    uint32_t availableBytes = 0;

    if(f_clientIsConnected) availableBytes = _client->available(); // available from stream
    else log_e("client not connected");
    // waiting for payload - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(f_waitingForPayload){
        if(availableBytes == 0){
            if(m_t0 + 3000 < millis()) {
                f_waitingForPayload = false;
                log_e("no payload received, timeout");
                stopSong();
                m_f_running = false;
            }
            return;
        }
        else {
            f_waitingForPayload = false;
        }
    }

    // chunked data tramsfer - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_chunked && availableBytes) {
        uint8_t readedBytes = 0;
        if(m_f_chunked && m_contentlength == byteCounter) {
            if(chunkSize > 0){
                if(_client->available() < 2) { // avoid getting out of sync
                    AUDIO_INFO("webfile chunked: not enough bytes available for skipCRLF");
                    return;
                }
                int a =_client->read(); if(a != 0x0D) log_w("chunk count error, expected: 0x0D, received: 0x%02X", a); // skipCR
                int b =_client->read(); if(b != 0x0A) log_w("chunk count error, expected: 0x0A, received: 0x%02X", b); // skipLF
            }
            chunkSize = readChunkSize(&readedBytes);
            if(chunkSize == 0) m_f_allDataReceived = true; // last chunk
            // log_w("chunk size: %d", chunkSize);
            m_contentlength += chunkSize;
            m_audioDataSize += chunkSize;
        }
        availableBytes = min(availableBytes, m_contentlength - byteCounter);
    }
    if(!m_f_chunked && byteCounter >= m_audioDataSize) {m_f_allDataReceived = true;}
    if(!f_clientIsConnected) {if(!m_f_allDataReceived)  m_f_allDataReceived = true;} // connection closed
    // log_w("byteCounter %u >= m_audioDataSize %u, m_f_allDataReceived % i", byteCounter, m_contentlength, m_f_allDataReceived);

    // if the buffer is often almost empty issue a warning - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_stream) {if(streamDetection(availableBytes)) return;}
    availableBytes = min(availableBytes, (uint32_t)InBuff.writeSpace());
    int32_t bytesAddedToBuffer = 0;
    if(f_clientIsConnected) bytesAddedToBuffer = _client->read(InBuff.getWritePtr(), availableBytes);
    if(bytesAddedToBuffer > 0) {
        m_webFilePos += bytesAddedToBuffer;
        byteCounter += bytesAddedToBuffer;
        if(m_f_chunked) m_chunkcount -= bytesAddedToBuffer;
        if(m_controlCounter == 100) audioDataCount += bytesAddedToBuffer;
        InBuff.bytesWritten(bytesAddedToBuffer);
    }

    // we have a webfile, read the file header first - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    if(m_controlCounter != 100) {
        if(InBuff.bufferFilled() > maxFrameSize || (m_contentlength && (InBuff.bufferFilled() == m_contentlength))) { // at least one complete frame or the file is smaller
            int32_t bytesRead = readAudioHeader(InBuff.getMaxAvailableBytes());
            if(bytesRead > 0) InBuff.bytesWasRead(bytesRead);
        }
        return;
    }

    if(m_codec == CODEC_OGG) { // log_i("determine correct codec here");
        uint8_t codec = determineOggCodec(InBuff.getReadPtr(), maxFrameSize);
        if     (codec == CODEC_FLAC)   {initializeDecoder(codec); m_codec = codec; return;}
        else if(codec == CODEC_OPUS)   {initializeDecoder(codec); m_codec = codec; return;}
        else if(codec == CODEC_VORBIS) {initializeDecoder(codec); m_codec = codec; return;}
        else {stopSong(); return;}
    }

    if(!m_f_stream && m_controlCounter == 100) {
        m_f_stream = true; // ready to play the audio data
        uint16_t filltime = millis() - m_t0;
        AUDIO_INFO("Webfile: stream ready, buffer filled in %d ms", filltime);
        return;
    }

    // end of webfile reached? - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_eof) { // m_f_eof and m_f_ID3v1TagFound will be set in playAudioData()
        if(m_f_ID3v1TagFound) readID3V1Tag();

        stopSong();
        if(m_f_tts) {
            AUDIO_INFO("End of speech \"%s\"", m_speechtxt.get());
            if(audio_eof_speech) audio_eof_speech(m_speechtxt.get());
        }
        else {
            AUDIO_INFO("End of webstream: \"%s\"", m_lastHost.get());
            if(audio_eof_stream) audio_eof_stream(m_lastHost.get());
        }
        return;
    }
    return;
}
//——————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————————
void Audio::processWebStreamTS() {
    uint32_t        availableBytes;     // available bytes in stream
    static bool     f_firstPacket;
    static bool     f_chunkFinished;
    static bool     f_nextRound;
    static uint32_t byteCounter;        // count received data
    static uint8_t  ts_packet[188];     // m3u8 transport stream is always 188 bytes long
    uint8_t         ts_packetStart = 0;
    uint8_t         ts_packetLength = 0;
    static uint8_t  ts_packetPtr = 0;
    const uint8_t   ts_packetsize = 188;
    static size_t   chunkSize = 0;

    // first call, set some values to default ———————————————————————————————————
    if(m_f_firstCall) {   // runs only ont time per connection, prepare for start
        m_f_firstCall = false;
        m_f_m3u8data = true;
        f_firstPacket = true;
        f_chunkFinished = false;
        f_nextRound = false;
        byteCounter = 0;
        chunkSize = 0;
        m_t0 = millis();
        ts_packetPtr = 0;
    } //—————————————————————————————————————————————————————————————————————————

    if(m_dataMode != AUDIO_DATA) return; // guard

nextRound:
    availableBytes = _client->available();
    if(availableBytes) {
        /* If the m3u8 stream uses 'chunked data transfer' no content length is supplied. Then the chunk size determines the audio data to be processed.
           However, the chunk size in some streams is limited to 32768 bytes, although the chunk can be larger. Then the chunk size is
           calculated again. The data used to calculate (here readedBytes) the chunk size is not part of it.
        */
        uint8_t readedBytes = 0;
        uint32_t minAvBytes = 0;
        if(m_f_chunked && chunkSize == byteCounter) {chunkSize += readChunkSize(&readedBytes); goto exit;}
        if(chunkSize) minAvBytes = min3(availableBytes, ts_packetsize - ts_packetPtr, chunkSize - byteCounter);
        else          minAvBytes = min(availableBytes, (uint32_t)(ts_packetsize - ts_packetPtr));

        int res = _client->read(ts_packet + ts_packetPtr, minAvBytes);
        if(res > 0) {
            ts_packetPtr += res;
            byteCounter += res;
            if(ts_packetPtr < ts_packetsize) return; // not enough data yet, the process must be repeated if the packet size (188 bytes) is not reached
            ts_packetPtr = 0;
            if(f_firstPacket) { // search for ID3 Header in the first packet
                f_firstPacket = false;
                uint8_t ID3_HeaderSize = process_m3u8_ID3_Header(ts_packet);
                if(ID3_HeaderSize > ts_packetsize) {
                    log_e("ID3 Header is too big");
                    stopSong();
                    return;
                }
                if(ID3_HeaderSize) {
                    memcpy(ts_packet, &ts_packet[ID3_HeaderSize], ts_packetsize - ID3_HeaderSize);
                    ts_packetPtr = ts_packetsize - ID3_HeaderSize;
                    return;
                }
            }
            ts_parsePacket(&ts_packet[0], &ts_packetStart, &ts_packetLength); // todo: check for errors

            if(ts_packetLength) {
                size_t ws = InBuff.writeSpace();
                if(ws >= ts_packetLength) {
                    memcpy(InBuff.getWritePtr(), ts_packet + ts_packetStart, ts_packetLength);
                    InBuff.bytesWritten(ts_packetLength);
                    f_nextRound = true;
                }
                else {
                    memcpy(InBuff.getWritePtr(), ts_packet + ts_packetStart, ws);
                    InBuff.bytesWritten(ws);
                    memcpy(InBuff.getWritePtr(), &ts_packet[ws + ts_packetStart], ts_packetLength - ws);
                    InBuff.bytesWritten(ts_packetLength - ws);
                }
            }
            if (byteCounter == m_contentlength || byteCounter == chunkSize) {
                f_chunkFinished = true;
                f_nextRound = false;
                byteCounter = 0;
                int av = _client->available();
                if(av == 7) for(int i = 0; i < av; i++) _client->read(); // waste last chunksize: 0x0D 0x0A 0x30 0x0D 0x0A 0x0D 0x0A (==0, end of chunked data transfer)
            }
            if(m_contentlength && byteCounter > m_contentlength) {log_e("byteCounter overflow, byteCounter: %d, contentlength: %d", byteCounter, m_contentlength); return;}
            if(chunkSize       && byteCounter > chunkSize)       {log_e("byteCounter overflow, byteCounter: %d, chunkSize: %d",     byteCounter, chunkSize); return;}
        }
    }
    if(f_chunkFinished) {
        if(m_f_psramFound) {
            if(InBuff.bufferFilled() < 60000) {
                f_chunkFinished = false;
                m_f_continue = true;
            }
        }
        else {
            f_chunkFinished = false;
            m_f_continue = true;
        }
    }

    // if the buffer is often almost empty issue a warning - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_stream) {
        if(streamDetection(availableBytes)) goto exit;
    }

    // buffer fill routine  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(true) {                                                  // statement has no effect
        if(InBuff.bufferFilled() > 60000 && !m_f_stream) {     // waiting for buffer filled
            m_f_stream = true;                                  // ready to play the audio data
            uint16_t filltime = millis() - m_t0;
            AUDIO_INFO("stream ready");
            AUDIO_INFO("buffer filled in %d ms", filltime);
        }
    }
    if(f_nextRound){goto nextRound;}
exit:
    return;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::processWebStreamHLS() {

    // first call, set some values to default - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(m_f_firstCall) { // runs only ont time per connection, prepare for start
        m_f_firstCall = false;
        m_f_m3u8data = true;
        m_t0 = millis();
        m_controlCounter = 0;

        pwsHLS.maxFrameSize = InBuff.getMaxBlockSize(); // every mp3/aac frame is not bigger
        pwsHLS.ID3BuffSize = 4096;
        pwsHLS.availableBytes = 0;
        pwsHLS.firstBytes = true;
        pwsHLS.f_chunkFinished = false;
        pwsHLS.byteCounter = 0;
        pwsHLS.chunkSize = 0;
        pwsHLS.ID3WritePtr = 0;
        pwsHLS.ID3ReadPtr = 0;
        pwsHLS.ID3Buff.alloc(pwsHLS.ID3BuffSize, "ID3Buff");
    }

    if(m_dataMode != AUDIO_DATA) return; // guard

    pwsHLS.availableBytes = _client->available();
    if(pwsHLS.availableBytes) { // an ID3 header could come here
        uint8_t readedBytes = 0;

        if(m_f_chunked && !pwsHLS.chunkSize) {
            pwsHLS.chunkSize = readChunkSize(&readedBytes);
            pwsHLS.byteCounter += readedBytes;
        }

        if(pwsHLS.firstBytes) {
            if(pwsHLS.ID3WritePtr < pwsHLS.ID3BuffSize) {
                pwsHLS.ID3WritePtr += _client->readBytes(&pwsHLS.ID3Buff[pwsHLS.ID3WritePtr], pwsHLS.ID3BuffSize - pwsHLS.ID3WritePtr);
                return;
            }
            if(m_controlCounter < 100) {
                int res = read_ID3_Header(&pwsHLS.ID3Buff[pwsHLS.ID3ReadPtr], pwsHLS.ID3BuffSize - pwsHLS.ID3ReadPtr);
                if(res >= 0) pwsHLS.ID3ReadPtr += res;
                if(pwsHLS.ID3ReadPtr > pwsHLS.ID3BuffSize) {
                    log_e("buffer overflow");
                    stopSong();
                    return;
                }
                return;
            }
            if(m_controlCounter != 100) return;

            size_t ws = InBuff.writeSpace();
            if(ws >= pwsHLS.ID3BuffSize - pwsHLS.ID3ReadPtr) {
                memcpy(InBuff.getWritePtr(), &pwsHLS.ID3Buff[pwsHLS.ID3ReadPtr], pwsHLS.ID3BuffSize - pwsHLS.ID3ReadPtr);
                InBuff.bytesWritten(pwsHLS.ID3BuffSize - pwsHLS.ID3ReadPtr);
            }
            else {
                memcpy(InBuff.getWritePtr(), &pwsHLS.ID3Buff[pwsHLS.ID3ReadPtr], ws);
                InBuff.bytesWritten(ws);
                memcpy(InBuff.getWritePtr(), &pwsHLS.ID3Buff[ws + pwsHLS.ID3ReadPtr], pwsHLS.ID3BuffSize - (pwsHLS.ID3ReadPtr + ws));
                InBuff.bytesWritten(pwsHLS.ID3BuffSize - (pwsHLS.ID3ReadPtr + ws));
            }
            pwsHLS.ID3Buff.reset();
            pwsHLS.byteCounter += pwsHLS.ID3BuffSize;
            pwsHLS.firstBytes = false;
        }

        size_t bytesWasWritten = 0;
        if(InBuff.writeSpace() >= pwsHLS.availableBytes) {
        //    if(availableBytes > 1024) availableBytes = 1024; // 1K throttle
            bytesWasWritten = _client->read(InBuff.getWritePtr(), pwsHLS.availableBytes);
        }
        else { bytesWasWritten = _client->read(InBuff.getWritePtr(), InBuff.writeSpace()); }
        InBuff.bytesWritten(bytesWasWritten);

        pwsHLS.byteCounter += bytesWasWritten;

        if(pwsHLS.byteCounter == m_contentlength || pwsHLS.byteCounter == pwsHLS.chunkSize) {
            pwsHLS.f_chunkFinished = true;
            pwsHLS.byteCounter = 0;
        }
    }

    if(pwsHLS.f_chunkFinished) {
        if(m_f_psramFound) {
            if(InBuff.bufferFilled() < 50000) {
                pwsHLS.f_chunkFinished = false;
                m_f_continue = true;
            }
        }
        else {
            pwsHLS.f_chunkFinished = false;
            m_f_continue = true;
        }
    }

    // if the buffer is often almost empty issue a warning or try a new connection - - - - - - - - - - - - - - - - - - -
    if(m_f_stream) {
        if(streamDetection(pwsHLS.availableBytes)) return;
    }

    if(InBuff.bufferFilled() > pwsHLS.maxFrameSize && !m_f_stream) { // waiting for buffer filled
        m_f_stream = true;                                    // ready to play the audio data
        //uint16_t filltime = millis() - m_t0;
        AUDIO_INFO("stream ready");
        // AUDIO_INFO("buffer filled in %u ms", filltime);
    }
    return;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::playAudioData() {

    if(!m_f_stream || m_f_eof || m_f_lockInBuffer || !m_f_running){return;} // guard, stream not ready or eof reached or InBuff is locked or not running
    if(m_dataMode == AUDIO_LOCALFILE && m_resumeFilePos != -1){    return;} // guard, m_resumeFilePos is set (-1 is default)
    if(m_validSamples) {playChunk();                               return;} // guard, play samples first
    //--------------------------------------------------------------------------------
    static uint8_t count = 0;
    static size_t oldAudioDataSize = 0;
    static bool   lastFrames = false;
    int32_t       bytesToDecode = InBuff.bufferFilled();
    int16_t       bytesDecoded = 0;

    if(m_f_firstPlayCall) {
        m_f_firstPlayCall = false;
        count = 0;
        oldAudioDataSize = 0;
        m_sumBytesDecoded = 0;
        m_bytesNotDecoded = 0;
        lastFrames = false;
        m_f_eof = false;
    }
    //--------------------------------------------------------------------------------

    m_f_audioTaskIsDecoding = true;

    if((m_dataMode == AUDIO_LOCALFILE || m_streamType == ST_WEBFILE) && m_playlistFormat != FORMAT_M3U8)  { // local file or webfile but not m3u8 file
        if(!m_audioDataSize) goto exit; // no data to decode if filesize is 0
        if(m_audioDataSize != oldAudioDataSize) { // Special case: Metadata in ogg files are recognized by the decoder,
            if(m_f_ogg)m_sumBytesDecoded = 0;     // after which m_audioDataStart and m_audioDataSize are updated.
            if(m_f_ogg)m_bytesNotDecoded = 0;
            oldAudioDataSize = m_audioDataSize;
        }

        bytesToDecode = m_audioDataSize - m_sumBytesDecoded;

        if(bytesToDecode < InBuff.getMaxBlockSize() * 2 && m_f_allDataReceived) { // last frames to decode
            lastFrames = true;
        }
        if(m_sumBytesDecoded > 0){
            if(m_sumBytesDecoded >= m_audioDataSize) {m_f_eof = true; goto exit;} // file end reached
            if(bytesToDecode <= 0)                   {m_f_eof = true; goto exit;} // file end reached
        }
    }
    else{if(InBuff.bufferFilled() < InBuff.getMaxBlockSize() && m_f_allDataReceived) {lastFrames = true;}}

    bytesToDecode = min((int32_t)InBuff.getMaxBlockSize(), bytesToDecode);

    if(lastFrames){
        bytesDecoded = sendBytes(InBuff.getReadPtr(), bytesToDecode);
    }
    else{
        if(InBuff.bufferFilled() >= InBuff.getMaxBlockSize()) bytesDecoded = sendBytes(InBuff.getReadPtr(), bytesToDecode);
        else bytesDecoded = 0; // Inbuff not filled enough
    }

    if(bytesDecoded <= 0) {
        if(lastFrames) {m_f_eof = true; goto exit;} // end of file reached
        count++;
        vTaskDelay(50); // wait for data
        if(count == 10) {if(m_f_allDataReceived) m_f_eof = true;}  // maybe slow stream
        goto exit; // syncword at pos0
    }
    count = 0;

    if(bytesDecoded > 0) {
        InBuff.bytesWasRead(bytesDecoded);
        if(m_controlCounter == 100) m_sumBytesDecoded += bytesDecoded;
        // log_w("m_audioDataSize: %d, m_sumBytesDecoded: %d, diff: %d", m_audioDataSize, m_sumBytesDecoded, m_audioDataSize - m_sumBytesDecoded);
        if(m_codec == CODEC_MP3 && m_audioDataSize - m_sumBytesDecoded == 128){
            m_f_ID3v1TagFound = true; m_f_eof = true;
        }
        if(m_f_allDataReceived && InBuff.bufferFilled() == 0) {
            m_f_eof = true; // end of file reached
        }
    }

exit:
    m_f_audioTaskIsDecoding = false;
    return;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::parseHttpResponseHeader() { // this is the response to a GET / request

    if(m_dataMode != HTTP_RESPONSE_HEADER) return false;
    if(!m_lastHost.valid()) {log_e("m_lastHost is empty"); return false;}

    uint32_t ctime = millis();
    uint32_t timeout = 4500; // ms

    static uint32_t stime;
    static bool     f_time = false;
    if(_client->available() == 0) {
        if(!f_time) {
            stime = millis();
            f_time = true;
        }
        if((millis() - stime) > timeout) {
            log_e("timeout");
            f_time = false;
            return false;
        }
    }
    f_time = false;

    ps_ptr<char>rhl;
    rhl.alloc(1024, "rhl"); // responseHeaderline
    rhl.clear();
    bool ct_seen = false;

    while(true) { // outer while
        uint16_t pos = 0;
        if((millis() - ctime) > timeout) {
            log_e("timeout");
            m_f_timeout = true;
            goto exit;
        }
        while(_client->available()) {
            uint8_t b = _client->read();
            if(b == '\n') {
                if(!pos) { // empty line received, is the last line of this responseHeader
                    if(ct_seen) goto lastToDo;
                    else {if(!_client->available())  goto exit;}
                }
                break;
            }
            if(b == '\r') rhl[pos] = 0;
            if(b < 0x20) continue;
            rhl[pos] = b;
            pos++;
            if(pos == 1023) {
                pos = 1022;
                continue;
            }
            if(pos == 1022) {
                rhl[pos] = '\0';
                log_w("responseHeaderline overflow");
            }
        } // inner while

        if(!pos) {
            vTaskDelay(5);
            continue;
        }
        if(rhl.starts_with_icase("HTTP/")) { // HTTP status error code
            char statusCode[5];
            statusCode[0] = rhl[9];
            statusCode[1] = rhl[10];
            statusCode[2] = rhl[11];
            statusCode[3] = '\0';
            int sc = atoi(statusCode);
            if(sc > 310) { // e.g. HTTP/1.1 301 Moved Permanently
                if(audio_showstreamtitle) audio_showstreamtitle(rhl.get());
                goto exit;
            }
        }

        else if(rhl.starts_with_icase("content-type:")) { // content-type: text/html; charset=UTF-8
            int idx = rhl.index_of(';', 13);
            if(idx > 0) rhl[idx] = '\0';
            if(parseContentType(rhl.get() + 13)) ct_seen = true;
            else{
                log_w("unknown contentType %s", rhl.get() + 13);
                goto exit;
            }
        }

        else if(rhl.starts_with_icase("location:")) {
            int pos = rhl.index_of_icase("http", 0);
            if(pos >= 0) {
                const char* c_host = (rhl.get() + pos);
                if(strcmp(c_host, m_lastHost.get()) != 0) { // prevent a loop
                    int pos_slash = indexOf(c_host, "/", 9);
                    if(pos_slash > 9) {
                        if(!strncmp(c_host, m_lastHost.get(), pos_slash)) {
                            AUDIO_INFO("redirect to new extension at existing host \"%s\"", c_host);
                            if(m_playlistFormat == FORMAT_M3U8) {
                                m_lastHost.assign(c_host);
                                m_f_m3u8data = true;
                            }
                            httpPrint(c_host);
                            while(_client->available()) _client->read(); // empty client buffer
                            return true;
                        }
                    }
                    AUDIO_INFO("redirect to new host \"%s\"", c_host);
                    m_f_reset_m3u8Codec = false;
                    connecttohost(c_host);
                    return true;
                }
            }
        }

        else if(rhl.starts_with_icase("content-encoding:")) {
            AUDIO_INFO("%s", rhl.get());
            if(rhl.contains("gzip")) {
                AUDIO_INFO("can't extract gzip");
                goto exit;
            }
        }

        else if(rhl.starts_with_icase("content-disposition:")) { // e.g we have this headerline:  content-disposition: attachment; filename=stream.asx
            int idx = rhl.index_of_icase("filename=");
            if (idx >= 0)  {
                ps_ptr<char> fn;
                fn.assign(rhl.get() + idx + 9); // Position directly after "filename="
                fn.replace("\"", ""); // remove '\"' around filename if present
                AUDIO_INFO("Filename is %s", fn.get());
            }
        }
        else if(rhl.starts_with_icase("connection:")) {
            if(rhl.contains("close")) { ; /* do nothing */ }
        }

        else if(rhl.starts_with_icase("icy-genre:")) {
            AUDIO_INFO("icy-genre: %s", rhl.get() + 10 ); // Ambient, Rock, etc
        }

        else if(rhl.starts_with_icase("icy-logo:")) {
            ps_ptr<char> icyLogo;
            icyLogo.assign(rhl.get() + 9); // Get logo URL
            icyLogo.trim();
            if(icyLogo.strlen() > 0){
                AUDIO_INFO("icy-logo: %s", icyLogo.get());
                if(audio_icylogo) audio_icylogo(icyLogo.get());
            }
        }

        else if(rhl.starts_with_icase("icy-br:")) {
            ps_ptr<char>c_bitRate; c_bitRate.assign(rhl.get() + 7);
            c_bitRate.trim();
            c_bitRate.append("000"); // Found bitrate tag, read the bitrate in Kbit
            setBitrate(c_bitRate.to_uint64());
            AUDIO_INFO("icy-bitrate: %s", c_bitRate.get());
            if(audio_bitrate) audio_bitrate(c_bitRate.get());
        }

        else if(rhl.starts_with_icase("icy-metaint:")) {
            const char* c_metaint = (rhl.get() + 12);
            int32_t     i_metaint = atoi(c_metaint);
            m_metaint = i_metaint;
            if(m_metaint) m_f_metadata = true; // Multimediastream
        }

        else if(rhl.starts_with_icase("icy-name:")) {
        // log_w("%s", rhl.get());
            ps_ptr<char> icyName;
            icyName.assign(rhl.get() + 9); // Get station name
            icyName.trim();
            if(icyName.strlen() > 0) {
                AUDIO_INFO("icy-name: %s", icyName.get());
                if(audio_showstation) audio_showstation(icyName.get());
            }
        }

        else if(rhl.starts_with_icase("content-length:")) {
            const char* c_cl = (rhl.get() + 15);
            int32_t     i_cl = atoi(c_cl);
            m_contentlength = i_cl;
            m_streamType = ST_WEBFILE; // Stream comes from a fileserver
            // AUDIO_INFO("content-length: %lu", (long unsigned int)m_contentlength);
        }

        else if(rhl.starts_with_icase("icy-description:")) {
            const char* c_idesc = (rhl.get() + 16);
            while(c_idesc[0] == ' ') c_idesc++;
            latinToUTF8(rhl); // if already UTF-8 do nothing, otherwise convert to UTF-8
            if(strlen(c_idesc) > 0 && specialIndexOf((uint8_t*)c_idesc, "24bit", 0) > 0) {
                AUDIO_INFO("icy-description: %s has to be 8 or 16", c_idesc);
                stopSong();
            }
            if(audio_icydescription) audio_icydescription(c_idesc);
        }

        else if(rhl.starts_with_icase("transfer-encoding:")) {
            if(rhl.ends_with_icase("chunked")) { // Station provides chunked transfer
                m_f_chunked = true;
                AUDIO_INFO("chunked data transfer");
                m_chunkcount = 0; // Expect chunkcount in DATA
            }
        }

        else if(rhl.starts_with("accept-ranges:")) {
            if(rhl.ends_with_icase("bytes")) m_f_acceptRanges = true;
        //    log_w("%s", rhl);
        }

        else if(rhl.starts_with("content-range:")) {
        //    log_w("%s", rhl);
        }

        else if(rhl.starts_with_icase("icy-url:")) {
            char* icyurl = (rhl.get() + 8);
            trim(icyurl);
            if(audio_icyurl) audio_icyurl(icyurl);
        }

        else if(rhl.starts_with_icase("www-authenticate:")) {
            AUDIO_INFO("authentification failed, wrong credentials?");
            goto exit;
        }
        else { ; }
    } // outer while

exit: // termination condition
    if(audio_showstation) audio_showstation("");
    if(audio_icydescription) audio_icydescription("");
    if(audio_icyurl) audio_icyurl("");
    m_dataMode = AUDIO_NONE;
    stopSong();
    return false;

lastToDo:
    m_streamType = ST_WEBSTREAM;
    if(m_contentlength > 0)          m_streamType = ST_WEBFILE;
    if(m_f_chunked)                  m_streamType = ST_WEBFILE; // Stream comes from a fileserver, metadata have webstreams
    if(m_f_chunked && m_f_metadata)  m_streamType = ST_WEBSTREAM;


    if(m_codec != CODEC_NONE) {
        m_dataMode = AUDIO_DATA; // Expecting data now
        if(!(m_codec == CODEC_OGG)){
            if(!initializeDecoder(m_codec)) return false;
        }
        // { log_i("Switch to DATA, metaint is %d", m_metaint); }
        if(m_playlistFormat != FORMAT_M3U8 && audio_lasthost) audio_lasthost(m_lastHost.get());
    }
    else if(m_playlistFormat != FORMAT_NONE) {
        m_dataMode = AUDIO_PLAYLISTINIT; // playlist expected
        // log_i("now parse playlist");
    }
    else {
        AUDIO_INFO("unknown content found at: %s", m_lastHost.get());
        goto exit;
    }
    return true;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::initializeDecoder(uint8_t codec) {
    uint32_t gfH = 0;
    uint32_t hWM = 0;
    switch(codec) {
        case CODEC_MP3:
            if(!MP3Decoder_IsInit()){
                if(!MP3Decoder_AllocateBuffers()) {
                    AUDIO_INFO("The MP3Decoder could not be initialized");
                    goto exit;
                }
                gfH = ESP.getFreeHeap();
                hWM = uxTaskGetStackHighWaterMark(NULL);
                AUDIO_INFO("MP3Decoder has been initialized, free Heap: %lu bytes , free stack %lu DWORDs", (long unsigned int)gfH, (long unsigned int)hWM);
                InBuff.changeMaxBlockSize(m_frameSizeMP3);
            }
            break;
        case CODEC_AAC:
            if(!AACDecoder_IsInit()) {
                if(!AACDecoder_AllocateBuffers()) {
                    AUDIO_INFO("The AACDecoder could not be initialized");
                    goto exit;
                }
                gfH = ESP.getFreeHeap();
                hWM = uxTaskGetStackHighWaterMark(NULL);
                AUDIO_INFO("AACDecoder has been initialized, free Heap: %lu bytes , free stack %lu DWORDs", (long unsigned int)gfH, (long unsigned int)hWM);
                InBuff.changeMaxBlockSize(m_frameSizeAAC);
            }
            break;
        case CODEC_M4A:
            if(!AACDecoder_IsInit()) {
                if(!AACDecoder_AllocateBuffers()) {
                    AUDIO_INFO("The AACDecoder could not be initialized");
                    goto exit;
                }
                gfH = ESP.getFreeHeap();
                hWM = uxTaskGetStackHighWaterMark(NULL);
                AUDIO_INFO("AACDecoder has been initialized, free Heap: %lu bytes , free stack %lu DWORDs", (long unsigned int)gfH, (long unsigned int)hWM);
                InBuff.changeMaxBlockSize(m_frameSizeAAC);
            }
            break;
        case CODEC_FLAC:
            if(!psramFound()) {
                AUDIO_INFO("FLAC works only with PSRAM!");
                goto exit;
            }
            if(!FLACDecoder_AllocateBuffers()) {
                AUDIO_INFO("The FLACDecoder could not be initialized");
                goto exit;
            }
            gfH = ESP.getFreeHeap();
            hWM = uxTaskGetStackHighWaterMark(NULL);
            InBuff.changeMaxBlockSize(m_frameSizeFLAC);
            AUDIO_INFO("FLACDecoder has been initialized, free Heap: %lu bytes , free stack %lu DWORDs", (long unsigned int)gfH, (long unsigned int)hWM);
            break;
        case CODEC_OPUS:
            if(!OPUSDecoder_AllocateBuffers()) {
                AUDIO_INFO("The OPUSDecoder could not be initialized");
                goto exit;
            }
            gfH = ESP.getFreeHeap();
            hWM = uxTaskGetStackHighWaterMark(NULL);
            AUDIO_INFO("OPUSDecoder has been initialized, free Heap: %lu bytes , free stack %lu DWORDs", (long unsigned int)gfH, (long unsigned int)hWM);
            InBuff.changeMaxBlockSize(m_frameSizeOPUS);
            break;
        case CODEC_VORBIS:
            if(!psramFound()) {
                AUDIO_INFO("VORBIS works only with PSRAM!");
                goto exit;
            }
            if(!VORBISDecoder_AllocateBuffers()) {
                AUDIO_INFO("The VORBISDecoder could not be initialized");
                goto exit;
            }
            gfH = ESP.getFreeHeap();
            hWM = uxTaskGetStackHighWaterMark(NULL);
            AUDIO_INFO("VORBISDecoder has been initialized, free Heap: %lu bytes,  free stack %lu DWORDs", (long unsigned int)gfH, (long unsigned int)hWM);
            InBuff.changeMaxBlockSize(m_frameSizeVORBIS);
            break;
        case CODEC_WAV: InBuff.changeMaxBlockSize(m_frameSizeWav); break;
        case CODEC_OGG: // the decoder will be determined later (vorbis, flac, opus?)
            break;
        default: goto exit; break;
    }
    return true;

exit:
    stopSong();
    return false;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// clang-format off
bool Audio::parseContentType(char* ct) {
    enum : int { CT_NONE, CT_MP3, CT_AAC, CT_M4A, CT_WAV, CT_FLAC, CT_PLS, CT_M3U, CT_ASX, CT_M3U8, CT_TXT, CT_AACP, CT_OPUS, CT_OGG, CT_VORBIS };

    strlower(ct);
    trim(ct);

    m_codec = CODEC_NONE;
    int ct_val = CT_NONE;

    if(!strcmp(ct, "audio/mpeg")) ct_val = CT_MP3;
    else if(!strcmp(ct, "audio/mpeg3"))                   ct_val = CT_MP3;
    else if(!strcmp(ct, "audio/x-mpeg"))                  ct_val = CT_MP3;
    else if(!strcmp(ct, "audio/x-mpeg-3"))                ct_val = CT_MP3;
    else if(!strcmp(ct, "audio/mp3"))                     ct_val = CT_MP3;
    else if(!strcmp(ct, "audio/aac"))                     ct_val = CT_AAC;
    else if(!strcmp(ct, "audio/x-aac"))                   ct_val = CT_AAC;
    else if(!strcmp(ct, "audio/aacp"))                    ct_val = CT_AAC;
    else if(!strcmp(ct, "video/mp2t")){                   ct_val = CT_AAC;  if(m_m3u8Codec == CODEC_MP3) ct_val = CT_MP3;} // see m3u8redirection()
    else if(!strcmp(ct, "audio/mp4"))                     ct_val = CT_M4A;
    else if(!strcmp(ct, "audio/m4a"))                     ct_val = CT_M4A;
    else if(!strcmp(ct, "audio/x-m4a"))                   ct_val = CT_M4A;
    else if(!strcmp(ct, "audio/wav"))                     ct_val = CT_WAV;
    else if(!strcmp(ct, "audio/x-wav"))                   ct_val = CT_WAV;
    else if(!strcmp(ct, "audio/flac"))                    ct_val = CT_FLAC;
    else if(!strcmp(ct, "audio/x-flac"))                  ct_val = CT_FLAC;
    else if(!strcmp(ct, "audio/scpls"))                   ct_val = CT_PLS;
    else if(!strcmp(ct, "audio/x-scpls"))                 ct_val = CT_PLS;
    else if(!strcmp(ct, "application/pls+xml"))           ct_val = CT_PLS;
    else if(!strcmp(ct, "audio/mpegurl")) {               ct_val = CT_M3U;  if(m_expectedPlsFmt == FORMAT_M3U8) ct_val = CT_M3U8;}
    else if(!strcmp(ct, "audio/x-mpegurl"))               ct_val = CT_M3U;
    else if(!strcmp(ct, "audio/ms-asf"))                  ct_val = CT_ASX;
    else if(!strcmp(ct, "video/x-ms-asf"))                ct_val = CT_ASX;
    else if(!strcmp(ct, "audio/x-ms-asx"))                ct_val = CT_ASX;  // #413
    else if(!strcmp(ct, "application/ogg"))               ct_val = CT_OGG;
    else if(!strcmp(ct, "audio/ogg"))                     ct_val = CT_OGG;
    else if(!strcmp(ct, "application/vnd.apple.mpegurl")) ct_val = CT_M3U8;
    else if(!strcmp(ct, "application/x-mpegurl"))         ct_val = CT_M3U8;
    else if(!strcmp(ct, "application/octet-stream"))      ct_val = CT_TXT;  // ??? listen.radionomy.com/1oldies before redirection
    else if(!strcmp(ct, "text/html"))                     ct_val = CT_TXT;
    else if(!strcmp(ct, "text/plain"))                    ct_val = CT_TXT;
    else if(ct_val == CT_NONE) {
        AUDIO_INFO("ContentType %s not supported", ct);
        return false; // nothing valid had been seen
    }
    else { ; }
    switch(ct_val) {
        case CT_MP3:
            m_codec = CODEC_MP3;
            // { log_i("ContentType %s, format is mp3", ct); } // ok is likely mp3
            break;
        case CT_AAC:
            m_codec = CODEC_AAC;
            // { log_i("ContentType %s, format is aac", ct); }
            break;
        case CT_M4A:
            m_codec = CODEC_M4A;
            // { log_i("ContentType %s, format is aac", ct); }
            break;
        case CT_FLAC:
            m_codec = CODEC_FLAC;
            // { log_i("ContentType %s, format is flac", ct); }
            break;
        case CT_OPUS:
            m_codec = CODEC_OPUS;
            m_f_ogg = true; // opus is ogg
            // { log_i("ContentType %s, format is opus", ct); }
            break;
        case CT_VORBIS:
            m_codec = CODEC_VORBIS;
            m_f_ogg = true; // vorbis is ogg
            log_i("ContentType %s, format is vorbis", ct);
            break;
        case CT_WAV:
            m_codec = CODEC_WAV;
            // { log_i("ContentType %s, format is wav", ct); }
            break;
        case CT_OGG:
            m_codec = CODEC_OGG; // determine in first OGG packet -OPUS, VORBIS, FLAC
            m_f_ogg = true; // maybe flac or opus or vorbis
            break;
        case CT_PLS: m_playlistFormat = FORMAT_PLS; break;
        case CT_M3U: m_playlistFormat = FORMAT_M3U; break;
        case CT_ASX: m_playlistFormat = FORMAT_ASX; break;
        case CT_M3U8: m_playlistFormat = FORMAT_M3U8; break;
        case CT_TXT: // overwrite text/plain
            if(m_expectedCodec == CODEC_AAC) {
                m_codec = CODEC_AAC;
                // log_i("set ct from M3U8 to AAC");
            }
            if(m_expectedCodec == CODEC_MP3) {
                m_codec = CODEC_MP3;
                // log_i("set ct from M3U8 to MP3");
            }

            if(m_expectedPlsFmt == FORMAT_ASX) {
                m_playlistFormat = FORMAT_ASX;
                // log_i("set playlist format to ASX");
            }
            if(m_expectedPlsFmt == FORMAT_M3U) {
                m_playlistFormat = FORMAT_M3U;
                // log_i("set playlist format to M3U");
            }
            if(m_expectedPlsFmt == FORMAT_M3U8) {
                m_playlistFormat = FORMAT_M3U8;
                // log_i("set playlist format to M3U8");
            }
            if(m_expectedPlsFmt == FORMAT_PLS) {
                m_playlistFormat = FORMAT_PLS;
                // log_i("set playlist format to PLS");
            }
            break;
        default:
            AUDIO_INFO("%s, unsupported audio format", ct);
            return false;
            break;
    }
    return true;
}
// clang-format on
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::showstreamtitle(char* ml) {
    // example for ml:
    // StreamTitle='Oliver Frank - Mega Hitmix';StreamUrl='www.radio-welle-woerthersee.at';
    // or adw_ad='true';durationMilliseconds='10135';adId='34254';insertionType='preroll';
    // html: 'Bielszy odcie&#324; bluesa 682 cz.1' --> 'Bielszy odcień bluesa 682 cz.1'

    if(!ml) return;
    ps_ptr<char>title = {};
    ps_ptr<char>artist = {};
    ps_ptr<char>streamTitle = {};
    ps_ptr<char>sUrl = {};

    htmlToUTF8(ml); // convert to UTF-8

    int16_t  idx1 = 0, idx2, idx4, idx5, idx6, idx7, titleLen = 0, artistLen = 0, titleStart = 0, artistStart = 0;

    // if(idx1 < 0) idx1 = indexOf(ml, "Title:", 0); // Title found (e.g. https://stream-hls.bauermedia.pt/comercial.aac/playlist.m3u8)
    // if(idx1 < 0) idx1 = indexOf(ml, "title:", 0); // Title found (e.g. #EXTINF:10,title="The Dan Patrick Show (M-F 9a-12p ET)",artist="zc1401"

    if(indexOf(ml, "StreamTitle='<?xml version=", 0) == 0) {
        /* e.g. xmlStreamTitle
              StreamTitle='<?xml version="1.0" encoding="utf-8"?><RadioInfo><Table><DB_ALBUM_ID>37364</DB_ALBUM_ID>
              <DB_ALBUM_IMAGE>00000037364.jpg</DB_ALBUM_IMAGE><DB_ALBUM_NAME>Boyfriend</DB_ALBUM_NAME>
              <DB_ALBUM_TYPE>Single</DB_ALBUM_TYPE><DB_DALET_ARTIST_NAME>DOVE CAMERON</DB_DALET_ARTIST_NAME>
              <DB_DALET_ITEM_CODE>CD4161</DB_DALET_ITEM_CODE><DB_DALET_TITLE_NAME>BOYFRIEND</DB_DALET_TITLE_NAME>
              <DB_FK_SITE_ID>2</DB_FK_SITE_ID><DB_IS_MUSIC>1</DB_IS_MUSIC><DB_LEAD_ARTIST_ID>26303</DB_LEAD_ARTIST_ID>
              <DB_LEAD_ARTIST_NAME>Dove Cameron</DB_LEAD_ARTIST_NAME><DB_RADIO_IMAGE>cidadefm.jpg</DB_RADIO_IMAGE>
              <DB_RADIO_NAME>Cidade</DB_RADIO_NAME><DB_SONG_ID>120126</DB_SONG_ID><DB_SONG_LYRIC>60981</DB_SONG_LYRIC>
              <DB_SONG_NAME>Boyfriend</DB_SONG_NAME></Table><AnimadorInfo><TITLE>Cidade</TITLE>
              <START_TIME_UTC>2022-11-15T22:00:00+00:00</START_TIME_UTC><END_TIME_UTC>2022-11-16T06:59:59+00:00
              </END_TIME_UTC><SHOW_NAME>Cidade</SHOW_NAME><SHOW_HOURS>22h às 07h</SHOW_HOURS><SHOW_PANEL>0</SHOW_PANEL>
              </AnimadorInfo></RadioInfo>';StreamUrl='';
        */
        idx4 = indexOf(ml, "<DB_DALET_TITLE_NAME>");
        idx5 = indexOf(ml, "</DB_DALET_TITLE_NAME>");
        idx6 = indexOf(ml, "<DB_LEAD_ARTIST_NAME>");
        idx7 = indexOf(ml, "</DB_LEAD_ARTIST_NAME>");
        if(idx4 == -1 || idx5 == -1) return;
        titleStart = idx4 + 21; // <DB_DALET_TITLE_NAME>
        titleLen = idx5 - titleStart;

        if(idx6 != -1 && idx7 != -1) {
            artistStart = idx6 + 21; // <DB_LEAD_ARTIST_NAME>
            artistLen = idx7 - artistStart;
        }
        if(titleLen) title.assign(ml + titleStart, titleLen);
        if(artistLen) artist.assign(ml + artistStart, artistLen);

        if(title.valid() && artist.valid()){
            streamTitle.assign(title.get());
            streamTitle.append(" - ");
            streamTitle.append(artist.get());
        }
        else if(title.valid()){
            streamTitle.assign(title.get());
        }
        else if(artist.valid()){
            streamTitle.assign(artist.get());
        }
    }

    else if(indexOf(ml, "StreamTitle='") == 0){
        titleStart = 13;
        idx2 = indexOf(ml, ";", 12);
        if(idx2 > titleStart + 1){
            titleLen = idx2 - 1 - titleStart;
            streamTitle.assign(ml + 13, titleLen);
        }
    }

    else if(startsWith(ml, "#EXTINF")){
        // extraxt StreamTitle from m3u #EXTINF line to icy-format
        // orig: #EXTINF:10,title="text="TitleName",artist="ArtistName"
        // conv: StreamTitle=TitleName - ArtistName
        // orig: #EXTINF:10,title="text=\"Spot Block End\" amgTrackId=\"9876543\"",artist=" ",url="length=\"00:00:00\""
        // conv: StreamTitle=text=\"Spot Block End\" amgTrackId=\"9876543\" -

        idx1 = indexOf(ml, "title=\"");
        idx2 = indexOf(ml, "artist=\"");
        if(idx1 > 0){
            int titleStart = idx1 + 7;
            int idx3 = indexOf(ml, "\"", titleStart);
            if(idx3 > titleStart){
                int titleLength = idx3 - titleStart;
                title.assign(ml + titleStart, titleLength);
                 if(title.starts_with("text=\\")){ // #EXTINF:10,title="text=\"Spot Block End\"",artist=" ",
                    titleStart += 7;
                    idx3 = indexOf(ml, "\\", titleStart);
                    if(idx3 > titleStart){
                        int titleLength = idx3 - titleStart;
                        title.assign(ml + titleStart, titleLength);
                    }
                }
            }
        }
        if(idx2 > 0){
            int artistStart = idx2 + 8;
            int idx3 = indexOf(ml, "\"", artistStart);
            if(idx3 > artistStart){
                int artistLength = idx3 - artistStart;
                artist.assign(ml + artistStart, artistLength);
                if(strcmp(artist.get(), " ") == 0) artist.reset();
            }
        }
        if(title.valid() && artist.valid()){
            streamTitle.assign(title.get());
            streamTitle.append(" - ");
            streamTitle.append(artist.get());
        }
        else if(title.valid()){
            streamTitle.assign(title.get());
        }
        else if(artist.valid()){
            streamTitle.assign(artist.get());
        }
    }
    else{
        ;
    }

    if(indexOf(ml, "StreamUrl=", 0) > 0){
        idx1 = indexOf(ml, "StreamUrl=", 0);
        idx2 = indexOf(ml, ";", idx1);
        if(idx1 >= 0 && idx2 > idx1) { // StreamURL found
            uint16_t len = idx2 - idx1;
            sUrl.assign(ml + idx1, len);
            if(sUrl.valid()){
                AUDIO_INFO("%s", sUrl.get());
            }
        }
    }

    if(indexOf(ml, "adw_ad=", 0) > 0){
        idx1 = indexOf(ml, "adw_ad=", 0);
        if(idx1 >= 0) { // Advertisement found
            idx1 = indexOf(ml, "durationMilliseconds=", 0);
            idx2 = indexOf(ml, ";", idx1);
            if(idx1 >= 0 && idx2 > idx1) {
                uint16_t len = idx2 - idx1;
                ps_ptr<char> sAdv;
                sAdv.assign(ml + idx1, len + 1);
                AUDIO_INFO("%s", sAdv.get());
                uint8_t pos = 21;                                                 // remove "StreamTitle="
                if(sAdv[pos] == '\'') pos++;                                      // remove leading  \'
                if(sAdv[strlen(sAdv.get()) - 1] == '\'') sAdv[strlen(sAdv.get()) - 1] = '\0'; // remove trailing \'
                if(audio_commercial) audio_commercial(sAdv.get() + pos);
            }
        }
    }

    if(streamTitle.valid()){
        if(!m_streamTitle.valid()){
            m_streamTitle.clone_from(streamTitle); // first init
            if(audio_showstreamtitle) audio_showstreamtitle(streamTitle.get());
        }
        if(!m_streamTitle.equals(streamTitle)){
            if(audio_showstreamtitle) audio_showstreamtitle(streamTitle.get());
            m_streamTitle.clone_from(streamTitle);
        }
    }
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::showCodecParams() {
    // print Codec Parameter (mp3, aac) in audio_info()

    AUDIO_INFO("Channels: %u", getChannels());
    AUDIO_INFO("SampleRate: %lu", getSampleRate());
    AUDIO_INFO("BitsPerSample: %u", getBitsPerSample());
    if(getBitRate()) { AUDIO_INFO("BitRate: %lu", getBitRate()); }
    else { AUDIO_INFO("BitRate: N/A"); }

    if(m_codec == CODEC_AAC) {
        uint8_t answ = AACGetFormat();
        if(answ < 3) {
            const char hf[4][8] = {"unknown", "ADIF", "ADTS"};
            AUDIO_INFO("AAC HeaderFormat: %s", hf[answ]);
        }
        answ = AACGetSBR();
        if(answ > 0 && answ < 4) {
            const char sbr[4][50] = {"without SBR", "upsampled SBR", "downsampled SBR", "no SBR used, but file is upsampled by a factor 2"};
            AUDIO_INFO("Spectral band replication: %s", sbr[answ]);
        }
    }
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int Audio::findNextSync(uint8_t* data, size_t len) {
    // Mp3 and aac audio data are divided into frames. At the beginning of each frame there is a sync word.
    // The sync word is 0xFFF. This is followed by information about the structure of the frame.
    // Wav files have no frames
    // Return: 0 the synchronous word was found at position 0
    //         > 0 is the offset to the next sync word
    //         -1 the sync word was not found within the block with the length len

    int         nextSync = 0;
    static uint32_t swnf = 0;
    if(m_codec == CODEC_WAV) {
        m_f_playing = true;
        nextSync = 0;
    }
    if(m_codec == CODEC_MP3) {
        nextSync = MP3FindSyncWord(data, len);
        if(nextSync == -1) return len; // syncword not found, search next block
    }
    if(m_codec == CODEC_AAC) { nextSync = AACFindSyncWord(data, len); }
    if(m_codec == CODEC_M4A) {
        if(!m_M4A_chConfig)m_M4A_chConfig = 2; // guard
        if(!m_M4A_sampleRate)m_M4A_sampleRate = 44100;
        if(!m_M4A_objectType)m_M4A_objectType = 2;
        AACSetRawBlockParams(m_M4A_chConfig, m_M4A_sampleRate, m_M4A_objectType);
        m_f_playing = true;
        nextSync = 0;
    }
    if(m_codec == CODEC_FLAC) {
        nextSync = FLACFindSyncWord(data, len);
        if(nextSync == -1) return len; // OggS not found, search next block
    }
    if(m_codec == CODEC_OPUS) {
        nextSync = OPUSFindSyncWord(data, len);
        if(nextSync == -1) return len; // OggS not found, search next block
    }
    if(m_codec == CODEC_VORBIS) {
        nextSync = VORBISFindSyncWord(data, len);
        if(nextSync == -1) return len; // OggS not found, search next block
    }
    if(nextSync == -1) {
        if(swnf == 0) AUDIO_INFO("syncword not found");
        else {
            swnf++; // syncword not found counter, can be multimediadata
        }
    }
    if(nextSync == 0) {
        if(swnf){
            AUDIO_INFO("syncword not found %lu times", (long unsigned int)swnf);
            swnf = 0;
        }
        else {
            AUDIO_INFO("syncword found at pos 0");
            m_f_decode_ready = true;
        }
    }
    if(nextSync > 0) { AUDIO_INFO("syncword found at pos %i", nextSync); }
    return nextSync;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::setDecoderItems() {
    if(m_codec == CODEC_MP3) {
        setChannels(MP3GetChannels());
        setSampleRate(MP3GetSampRate());
        setBitsPerSample(MP3GetBitsPerSample());
        setBitrate(MP3GetBitrate());
        AUDIO_INFO("MPEG-%s, Layer %s",(MP3GetVersion()==0) ? "2.5" : (MP3GetVersion()==2) ? "2" : "1", (MP3GetLayer()==1) ? "III" : (MP3GetLayer()==2) ? "II" : "I");
    }
    if(m_codec == CODEC_AAC || m_codec == CODEC_M4A) {
        setChannels(AACGetChannels());
        setSampleRate(AACGetSampRate());
        setBitsPerSample(AACGetBitsPerSample());
        setBitrate(AACGetBitrate());
    }
    if(m_codec == CODEC_FLAC) {
        setChannels(FLACGetChannels());
        setSampleRate(FLACGetSampRate());
        setBitsPerSample(FLACGetBitsPerSample());
        setBitrate(FLACGetBitRate());
        if(FLACGetAudioDataStart() > 0){ // only flac-ogg, native flac sets audioDataStart in readFlacHeader()
            m_audioDataStart = FLACGetAudioDataStart();
            if(getFileSize()) m_audioDataSize = getFileSize() - m_audioDataStart;
        }
    }
    if(m_codec == CODEC_OPUS) {
        setChannels(OPUSGetChannels());
        setSampleRate(OPUSGetSampRate());
        setBitsPerSample(OPUSGetBitsPerSample());
        setBitrate(OPUSGetBitRate());
        if(OPUSGetAudioDataStart() > 0){
            m_audioDataStart = OPUSGetAudioDataStart();
            if(getFileSize()) m_audioDataSize = getFileSize() - m_audioDataStart;
        }
    }
    if(m_codec == CODEC_VORBIS) {
        setChannels(VORBISGetChannels());
        setSampleRate(VORBISGetSampRate());
        setBitsPerSample(VORBISGetBitsPerSample());
        setBitrate(VORBISGetBitRate());
        if(VORBISGetAudioDataStart() > 0){
            m_audioDataStart = VORBISGetAudioDataStart();
            if(getFileSize()) m_audioDataSize = getFileSize() - m_audioDataStart;
        }
    }
    if(getBitsPerSample() != 8 && getBitsPerSample() != 16) {
        AUDIO_INFO("Bits per sample must be 8 or 16, found %i", getBitsPerSample());
        stopSong();
    }
    if(getChannels() != 1 && getChannels() != 2) {
        AUDIO_INFO("Num of channels must be 1 or 2, found %i", getChannels());
        stopSong();
    }
    memset(m_filterBuff, 0, sizeof(m_filterBuff)); // Clear FilterBuffer
    IIR_calculateCoefficients(m_gain0, m_gain1, m_gain2); // must be recalculated after each samplerate change
    showCodecParams();
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int Audio::sendBytes(uint8_t* data, size_t len) {
    if(!m_f_running) return 0; // guard
    int32_t     bytesLeft;
    static bool f_setDecodeParamsOnce = true;
    int         nextSync = 0;
    if(!m_f_playing) {
        f_setDecodeParamsOnce = true;
        nextSync = findNextSync(data, len);
        if(nextSync <  0) return len;
        if(nextSync == 0) { m_f_playing = true; }
        if(nextSync >  0) return nextSync;
    }
    // m_f_playing is true at this pos
    bytesLeft = len;
    m_decodeError = 0;
    int bytesDecoded = 0;

    if(m_codec == CODEC_NONE && m_playlistFormat == FORMAT_M3U8) return 0; // can happen when the m3u8 playlist is loaded
    if(!m_f_decode_ready) return 0; // find sync first

    switch(m_codec) {
        case CODEC_WAV:    m_decodeError = 0; bytesLeft = 0; break;
        case CODEC_MP3:    m_decodeError = MP3Decode(   data, &bytesLeft, m_outBuff.get(), 0); break;
        case CODEC_AAC:    m_decodeError = AACDecode(   data, &bytesLeft, m_outBuff.get()); break;
        case CODEC_M4A:    m_decodeError = AACDecode(   data, &bytesLeft, m_outBuff.get()); break;
        case CODEC_FLAC:   m_decodeError = FLACDecode(  data, &bytesLeft, m_outBuff.get()); break;
        case CODEC_OPUS:   m_decodeError = OPUSDecode(  data, &bytesLeft, m_outBuff.get()); break;
        case CODEC_VORBIS: m_decodeError = VORBISDecode(data, &bytesLeft, m_outBuff.get()); break;
        default: {
            log_e("no valid codec found codec = %d", m_codec);
            stopSong();
        }
    }

    // m_decodeError - possible values are:
    //                   0: okay, no error
    //                 100: the decoder needs more data
    //                 < 0: there has been an error

    if(m_decodeError < 0) { // Error, skip the frame...
        if((m_codec == CODEC_MP3) && (m_f_chunked == true)){ // http://bestof80s.stream.laut.fm/best_of_80s
            // log_e("%02X %02X %02X %02X %02X %02X %02X %02X %02X", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
            if(specialIndexOf(data, "ID3", 4) == 0){
                uint16_t  id3Size = bigEndian(data + 6, 4, 7);
                id3Size += 10;
                AUDIO_INFO("ID3 tag found, skip %i bytes", id3Size);
                return id3Size; // skip ID3 tag
            }
            if(m_decodeError == ERR_MP3_INVALID_HUFFCODES) {
                AUDIO_INFO("last mp3 frame is invalid");
                MP3Decoder_ClearBuffer();
                return findNextSync(data, bytesLeft); // skip last mp3 frame and search for next syncword
            }
        }
        //  According to the specification, the channel configuration is transferred in the first ADTS header and no longer changes in the entire
        //  stream. Some streams send short mono blocks in a stereo stream. e.g. http://mp3.ffh.de/ffhchannels/soundtrack.aac
        //  This triggers error -21 because the faad2 decoder cannot switch automatically.
        if(m_codec == CODEC_AAC && m_decodeError == -21){ // mono <-> stereo change
            static uint8_t channels = 0;
            if ((data[0] == 0xFF) || ((data[1] & 0xF0) == 0xF0)){
                int channel_config = ((data[2] & 0x01) << 2) | ((data[3] & 0xC0) >> 6);
                if(channel_config != channels) {
                    channels = channel_config;
                    AUDIO_INFO("AAC channel config changed to %d", channels);
                }
            }
            AACDecoder_FreeBuffers();
            AACDecoder_AllocateBuffers();
            return 0;
        }

        printDecodeError(m_decodeError);
        m_f_playing = false; // seek for new syncword
        if(m_codec == CODEC_FLAC) {
        //    if(m_decodeError == ERR_FLAC_BITS_PER_SAMPLE_TOO_BIG) stopSong();
        //    if(m_decodeError == ERR_FLAC_RESERVED_CHANNEL_ASSIGNMENT) stopSong();
        }
        if(m_codec == CODEC_OPUS) {
            if(m_decodeError == ERR_OPUS_HYBRID_MODE_UNSUPPORTED) stopSong();
            if(m_decodeError == ERR_OPUS_SILK_MODE_UNSUPPORTED) stopSong();
            if(m_decodeError == ERR_OPUS_NARROW_BAND_UNSUPPORTED) stopSong();
            if(m_decodeError == ERR_OPUS_WIDE_BAND_UNSUPPORTED) stopSong();
            if(m_decodeError == ERR_OPUS_SUPER_WIDE_BAND_UNSUPPORTED) stopSong();
            if(m_decodeError == ERR_OPUS_INVALID_SAMPLERATE) stopSong();
            return 0;
        }
        return 1; // skip one byte and seek for the next sync word
    }
    bytesDecoded = len - bytesLeft;

    if(bytesDecoded == 0 && m_decodeError == 0) { // unlikely framesize
        AUDIO_INFO("framesize is 0, start decoding again");
        m_f_playing = false; // seek for new syncword
        // we're here because there was a wrong sync word so skip one byte and seek for the next
        return 1;
    }
    // status: bytesDecoded > 0 and m_decodeError >= 0
    char* st = NULL;
    std::vector<uint32_t> vec;
    switch(m_codec) {
        case CODEC_WAV:     if(getBitsPerSample() == 16){
                                memmove(m_outBuff.get(), data, len); // copy len data in outbuff and set validsamples and bytesdecoded=len
                                m_validSamples = len / (2 * getChannels());
                            }
                            else{
                                for(int i = 0; i < len; i++) {
                                    int16_t sample1 = (data[i] & 0x00FF)      - 128;
                                    int16_t sample2 = (data[i] & 0xFF00 >> 8) - 128;
                                    m_outBuff[i * 2 + 0] = sample1 << 8;
                                    m_outBuff[i * 2 + 1] = sample2 << 8;
                                }
                                m_validSamples = len;
                            }
                            break;
        case CODEC_MP3:     m_validSamples = MP3GetOutputSamps() / getChannels();
                            break;
        case CODEC_AAC:     m_validSamples = AACGetOutputSamps() / getChannels();
                            static uint8_t isPS = 0;
                            if(!isPS && AACGetParametricStereo()){ // only change 0 -> 1
                                isPS = 1;
                                AUDIO_INFO("Parametric Stereo");
                            }
                            else isPS = AACGetParametricStereo();
                            break;
        case CODEC_M4A:     m_validSamples = AACGetOutputSamps() / getChannels();
                            break;
        case CODEC_FLAC:    if(m_decodeError == FLAC_PARSE_OGG_DONE) return bytesDecoded; // nothing to play
                            m_validSamples = FLACGetOutputSamps() / getChannels();
                            st = FLACgetStreamTitle();
                            if(st) {
                                AUDIO_INFO(st);
                                if(audio_showstreamtitle) audio_showstreamtitle(st);
                            }
#ifndef AUDIO_NO_SD_FS
                            vec = FLACgetMetadataBlockPicture();
                            if(vec.size() > 0){ // get blockpic data
                                // log_i("---------------------------------------------------------------------------");
                                // log_i("ogg metadata blockpicture found:");
                                // for(int i = 0; i < vec.size(); i += 2) { log_i("segment %02i, pos %07i, len %05i", i / 2, vec[i], vec[i + 1]); }
                                // log_i("---------------------------------------------------------------------------");
                                if(audio_oggimage) audio_oggimage(m_audiofile, vec);
                            }
#endif  // AUDIO_NO_SD_FS	
                            break;
        case CODEC_OPUS:    if(m_decodeError == OPUS_PARSE_OGG_DONE) return bytesDecoded; // nothing to play
                            if(m_decodeError == OPUS_END)            return bytesDecoded; // nothing to play
                            m_validSamples = OPUSGetOutputSamps();
                            st = OPUSgetStreamTitle();
                            if(st){
                                AUDIO_INFO(st);
                                if(audio_showstreamtitle) audio_showstreamtitle(st);
                            }
#ifndef AUDIO_NO_SD_FS
                            vec = OPUSgetMetadataBlockPicture();
                            if(vec.size() > 0){ // get blockpic data
                                // log_i("---------------------------------------------------------------------------");
                                // log_i("ogg metadata blockpicture found:");
                                // for(int i = 0; i < vec.size(); i += 2) { log_i("segment %02i, pos %07i, len %05i", i / 2, vec[i], vec[i + 1]); }
                                // log_i("---------------------------------------------------------------------------");
                                if(audio_oggimage) audio_oggimage(m_audiofile, vec);
                            }

                            if(m_opus_mode != OPUSgetMode()){
                                m_opus_mode = OPUSgetMode();
                                if(m_opus_mode == MODE_CELT_ONLY) AUDIO_INFO("Opus Mode: CELT_ONLY");
                                if(m_opus_mode == MODE_HYBRID)    AUDIO_INFO("Opus Mode: HYBRID");
                                if(m_opus_mode == MODE_SILK_ONLY) AUDIO_INFO("Opus Mode: SILK_ONLY");
                                if(m_opus_mode == MODE_NONE)      AUDIO_INFO("Opus Mode: NONE");
                            }
#endif
                            break;

        case CODEC_VORBIS:  if(m_decodeError == VORBIS_PARSE_OGG_DONE) return bytesDecoded; // nothing to play
                            m_validSamples = VORBISGetOutputSamps();
                            st = VORBISgetStreamTitle();
                            if(st) {
                                AUDIO_INFO(st);
                                if(audio_showstreamtitle) audio_showstreamtitle(st);
                            }
#ifndef AUDIO_NO_SD_FS
                            vec = VORBISgetMetadataBlockPicture();
                            if(vec.size() > 0){ // get blockpic data
                                // log_i("---------------------------------------------------------------------------");
                                // log_i("ogg metadata blockpicture found:");
                                // for(int i = 0; i < vec.size(); i += 2) { log_i("segment %02i, pos %07i, len %05i", i / 2, vec[i], vec[i + 1]); }
                                // log_i("---------------------------------------------------------------------------");
                                if(audio_oggimage) audio_oggimage(m_audiofile, vec);
                            }
#endif
                            break;
    }
    if(f_setDecodeParamsOnce && m_validSamples) {
        f_setDecodeParamsOnce = false;
        setDecoderItems();
        m_PlayingStartTime = millis();
    }

    uint16_t bytesDecoderOut = m_validSamples;
    if(m_channels == 2) bytesDecoderOut /= 2;
    if(m_bitsPerSample == 16) bytesDecoderOut *= 2;
    computeAudioTime(bytesDecoded, bytesDecoderOut);

    m_curSample = 0;
    playChunk();
    return bytesDecoded;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::computeAudioTime(uint16_t bytesDecoderIn, uint16_t bytesDecoderOut) {

    if(m_dataMode != AUDIO_LOCALFILE && m_streamType != ST_WEBFILE) return; //guard

    static uint64_t sumBytesIn         = 0;
    static uint64_t sumBytesOut        = 0;
    static uint32_t sumBitRate         = 0;
    static uint32_t counter            = 0;
    static uint32_t timeStamp          = 0;
    static uint32_t deltaBytesIn       = 0;
    static uint32_t nominalBitRate     = 0;

    if(m_f_firstCurTimeCall) { // first call
        m_f_firstCurTimeCall = false;
        sumBytesIn = 0;
        sumBytesOut = 0;
        sumBitRate  = 0;
        counter = 0;
        timeStamp = millis();
        deltaBytesIn = 0;
        nominalBitRate = 0;

        if(m_codec == CODEC_FLAC && FLACGetAudioFileDuration()){
            m_audioFileDuration = FLACGetAudioFileDuration();
            nominalBitRate = (m_audioDataSize / FLACGetAudioFileDuration()) * 8;
            m_avr_bitrate = nominalBitRate;
        }
        if(m_codec == CODEC_WAV){
            nominalBitRate = getBitRate();
            m_avr_bitrate = nominalBitRate;
            m_audioFileDuration = m_audioDataSize  / (getSampleRate() * getChannels());
            if(getBitsPerSample() == 16) m_audioFileDuration /= 2;
        }
    }

    sumBytesIn   += bytesDecoderIn;
    deltaBytesIn += bytesDecoderIn;
    sumBytesOut  += bytesDecoderOut;


    if(timeStamp + 500 < millis()){
        uint32_t t       = millis();      // time tracking
        uint32_t delta_t = t - timeStamp; //    ---"---
        timeStamp = t;                    //    ---"---

        uint32_t bitRate = ((deltaBytesIn * 8000) / delta_t);  // we know the time and bytesIn to compute the bitrate

        sumBitRate += bitRate;
        counter ++;
        if(nominalBitRate){
            m_audioCurrentTime = round(((float)sumBytesIn * 8) / m_avr_bitrate);
        }
        else{
            m_avr_bitrate = sumBitRate / counter;
            m_audioCurrentTime = (sumBytesIn * 8) / m_avr_bitrate;
            m_audioFileDuration = round(((float)m_audioDataSize * 8 / m_avr_bitrate));
        }
        deltaBytesIn = 0;
    }

    if(m_haveNewFilePos && m_avr_bitrate){
        uint32_t posWhithinAudioBlock =  m_haveNewFilePos;
        uint32_t newTime = posWhithinAudioBlock / (m_avr_bitrate / 8);
        m_audioCurrentTime = newTime;
        sumBytesIn = posWhithinAudioBlock;
        m_haveNewFilePos = 0;
    }
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::printDecodeError(int r) {
    const char* e;

    if(m_codec == CODEC_MP3) {
        switch(r) {
            case ERR_MP3_NONE: e = "NONE"; break;
            case ERR_MP3_INDATA_UNDERFLOW: e = "INDATA_UNDERFLOW"; break;
            case ERR_MP3_MAINDATA_UNDERFLOW: e = "MAINDATA_UNDERFLOW"; break;
            case ERR_MP3_FREE_BITRATE_SYNC: e = "FREE_BITRATE_SYNC"; break;
            case ERR_MP3_OUT_OF_MEMORY: e = "OUT_OF_MEMORY"; break;
            case ERR_MP3_NULL_POINTER: e = "NULL_POINTER"; break;
            case ERR_MP3_INVALID_FRAMEHEADER: e = "INVALID_FRAMEHEADER"; break;
            case ERR_MP3_INVALID_SIDEINFO: e = "INVALID_SIDEINFO"; break;
            case ERR_MP3_INVALID_SCALEFACT: e = "INVALID_SCALEFACT"; break;
            case ERR_MP3_INVALID_HUFFCODES: e = "INVALID_HUFFCODES"; break;
            case ERR_MP3_INVALID_DEQUANTIZE: e = "INVALID_DEQUANTIZE"; break;
            case ERR_MP3_INVALID_IMDCT: e = "INVALID_IMDCT"; break;
            case ERR_MP3_INVALID_SUBBAND: e = "INVALID_SUBBAND"; break;
            default: e = "ERR_UNKNOWN";
        }
        AUDIO_INFO("MP3 decode error %d : %s", r, e);
    }
    if(m_codec == CODEC_AAC || m_codec == CODEC_M4A) {
        e = AACGetErrorMessage(abs(r));
        AUDIO_INFO("AAC decode error %d : %s", r, e);
    }
    if(m_codec == CODEC_FLAC) {
        switch(r) {
            case ERR_FLAC_NONE: e = "NONE"; break;
            case ERR_FLAC_BLOCKSIZE_TOO_BIG: e = "BLOCKSIZE TOO BIG"; break;
            case ERR_FLAC_RESERVED_BLOCKSIZE_UNSUPPORTED: e = "Reserved Blocksize unsupported"; break;
            case ERR_FLAC_SYNC_CODE_NOT_FOUND: e = "SYNC CODE NOT FOUND"; break;
            case ERR_FLAC_UNKNOWN_CHANNEL_ASSIGNMENT: e = "UNKNOWN CHANNEL ASSIGNMENT"; break;
            case ERR_FLAC_RESERVED_CHANNEL_ASSIGNMENT: e = "RESERVED CHANNEL ASSIGNMENT"; break;
            case ERR_FLAC_RESERVED_SUB_TYPE: e = "RESERVED SUB TYPE"; break;
            case ERR_FLAC_PREORDER_TOO_BIG: e = "PREORDER TOO BIG"; break;
            case ERR_FLAC_RESERVED_RESIDUAL_CODING: e = "RESERVED RESIDUAL CODING"; break;
            case ERR_FLAC_WRONG_RICE_PARTITION_NR: e = "WRONG RICE PARTITION NR"; break;
            case ERR_FLAC_BITS_PER_SAMPLE_TOO_BIG: e = "BITS PER SAMPLE > 16"; break;
            case ERR_FLAC_BITS_PER_SAMPLE_UNKNOWN: e = "BITS PER SAMPLE UNKNOWN"; break;
            case ERR_FLAC_DECODER_ASYNC: e = "DECODER ASYNCHRON"; break;
            case ERR_FLAC_BITREADER_UNDERFLOW: e = "BITREADER ERROR"; break;
            case ERR_FLAC_OUTBUFFER_TOO_SMALL: e = "OUTBUFFER TOO SMALL"; break;
            default: e = "ERR_UNKNOWN";
        }
        AUDIO_INFO("FLAC decode error %d : %s", r, e);
    }
    if(m_codec == CODEC_OPUS) {
        switch(r) {
            case ERR_OPUS_NONE: e = "NONE"; break;
            case ERR_OPUS_CHANNELS_OUT_OF_RANGE: e = "UNKNOWN CHANNEL ASSIGNMENT"; break;
            case ERR_OPUS_INVALID_SAMPLERATE: e = "SAMPLERATE IS NOT 48000Hz"; break;
            case ERR_OPUS_EXTRA_CHANNELS_UNSUPPORTED: e = "EXTRA CHANNELS UNSUPPORTED"; break;
            case ERR_OPUS_SILK_MODE_UNSUPPORTED: e = "SILK MODE UNSUPPORTED"; break;
            case ERR_OPUS_HYBRID_MODE_UNSUPPORTED: e = "HYBRID MODE UNSUPPORTED"; break;
            case ERR_OPUS_NARROW_BAND_UNSUPPORTED: e = "NARROW_BAND_UNSUPPORTED"; break;
            case ERR_OPUS_WIDE_BAND_UNSUPPORTED: e = "WIDE_BAND_UNSUPPORTED"; break;
            case ERR_OPUS_SUPER_WIDE_BAND_UNSUPPORTED: e = "SUPER_WIDE_BAND_UNSUPPORTED"; break;
            case ERR_OPUS_CELT_BAD_ARG: e = "CELT_DECODER_BAD_ARG"; break;
            case ERR_OPUS_CELT_INTERNAL_ERROR: e = "CELT DECODER INTERNAL ERROR"; break;
            case ERR_OPUS_CELT_UNIMPLEMENTED: e = "CELT DECODER UNIMPLEMENTED ARG"; break;
            case ERR_OPUS_CELT_ALLOC_FAIL: e = "CELT DECODER INIT ALLOC FAIL"; break;
            case ERR_OPUS_CELT_UNKNOWN_REQUEST: e = "CELT_UNKNOWN_REQUEST FAIL"; break;
            case ERR_OPUS_CELT_GET_MODE_REQUEST: e = "CELT_GET_MODE_REQUEST FAIL"; break;
            case ERR_OPUS_CELT_CLEAR_REQUEST: e = "CELT_CLEAR_REAUEST_FAIL"; break;
            case ERR_OPUS_CELT_SET_CHANNELS: e = "CELT_SET_CHANNELS_FAIL"; break;
            case ERR_OPUS_CELT_END_BAND: e = "CELT_END_BAND_REQUEST_FAIL"; break;
            case ERR_CELT_OPUS_INTERNAL_ERROR: e = "CELT_INTERNAL_ERROR"; break;
            default: e = "ERR_UNKNOWN";
        }
        AUDIO_INFO("OPUS decode error %d : %s", r, e);
    }
    if(m_codec == CODEC_VORBIS) {
        switch(r) {
            case ERR_VORBIS_NONE: e = "NONE"; break;
            case ERR_VORBIS_CHANNELS_OUT_OF_RANGE: e = "CHANNELS OUT OF RANGE"; break;
            case ERR_VORBIS_INVALID_SAMPLERATE: e = "INVALID SAMPLERATE"; break;
            case ERR_VORBIS_EXTRA_CHANNELS_UNSUPPORTED: e = "EXTRA CHANNELS UNSUPPORTED"; break;
            case ERR_VORBIS_DECODER_ASYNC: e = "DECODER ASYNC"; break;
            case ERR_VORBIS_OGG_SYNC_NOT_FOUND: e = "SYNC NOT FOUND"; break;
            case ERR_VORBIS_BAD_HEADER: e = "BAD HEADER"; break;
            case ERR_VORBIS_NOT_AUDIO: e = "NOT AUDIO"; break;
            case ERR_VORBIS_BAD_PACKET: e = "BAD PACKET"; break;
            default: e = "ERR_UNKNOWN";
        }
        AUDIO_INFO("VORBIS decode error %d : %s", r, e);
    }
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::setPinout(uint8_t BCLK, uint8_t LRC, uint8_t DOUT, int8_t MCLK) {

    m_f_psramFound = psramInit();

    m_outBuff.alloc(m_outbuffSize * sizeof(int16_t), "m_outbuff");
    m_samplesBuff48K.alloc(m_samplesBuff48KSize * sizeof(int16_t), "m_samplesBuff48K");

    esp_err_t result = ESP_OK;

#if(ESP_ARDUINO_VERSION_MAJOR < 3)
    log_e("Arduino Version must be 3.0.0 or higher!");
#endif
    trim(audioI2SVers);
    AUDIO_INFO("audioI2S %s", audioI2SVers);

    i2s_std_gpio_config_t gpio_cfg = {};
    gpio_cfg.bclk = (gpio_num_t)BCLK;
    gpio_cfg.din = (gpio_num_t)I2S_GPIO_UNUSED;
    gpio_cfg.dout = (gpio_num_t)DOUT;
    gpio_cfg.mclk = (gpio_num_t)MCLK;
    gpio_cfg.ws = (gpio_num_t)LRC;
    I2Sstop();
    result = i2s_channel_reconfig_std_gpio(m_i2s_tx_handle, &gpio_cfg);
    I2Sstart();

    return (result == ESP_OK);
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::getFileSize() { // returns the size of webfile or local file
#ifdef AUDIO_NO_SD_FS
  return 0;
#else	    
    if(!m_audiofile) {
        if (m_contentlength > 0) { return m_contentlength;}
        return 0;
    }
    return m_audiofile.size();
#endif						 
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::getFilePos() {
#ifdef AUDIO_NO_SD_FS
  return 0;
#else
    if(m_dataMode == AUDIO_LOCALFILE){
        if(!m_audiofile) return 0;
        return m_audiofile.position();
    }
    if(m_streamType == ST_WEBFILE){
        return m_webFilePos;
    }
    return 0;
#endif
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::getAudioDataStartPos() {
#ifdef AUDIO_NO_SD_FS
  return 0;
#else
    if(m_dataMode == AUDIO_LOCALFILE){
        if(!m_audiofile) return 0;
        return m_audioDataStart;
    }
    if(m_streamType == ST_WEBFILE){
        return m_audioDataStart;
    }
    return 0;
#endif  // AUDIO_NO_SD_FS
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::getAudioFileDuration() {
    if(!m_avr_bitrate)                                      return 0;
    if(m_playlistFormat == FORMAT_M3U8)                     return 0;
    if(m_dataMode == AUDIO_LOCALFILE) {if(!m_audioDataSize) return 0;}
    if(m_streamType == ST_WEBFILE)    {if(!m_contentlength) return 0;}
    return m_audioFileDuration;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::getAudioCurrentTime() { // return current time in seconds
    return round(m_audioCurrentTime);
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::setAudioPlayPosition(uint16_t sec) {
    if(!m_f_psramFound) {               log_w("PSRAM must be activated"); return false;} // guard
    if(m_dataMode != AUDIO_LOCALFILE /* && m_streamType == ST_WEBFILE */) return false;  // guard
    if(!m_avr_bitrate)                                                    return false;  // guard
    //if(m_codec == CODEC_OPUS) return false;   // not impl. yet
    //if(m_codec == CODEC_VORBIS) return false; // not impl. yet
    // Jump to an absolute position in time within an audio file
    // e.g. setAudioPlayPosition(300) sets the pointer at pos 5 min
    if(sec > getAudioFileDuration()) sec = getAudioFileDuration();
    uint32_t filepos = m_audioDataStart + (m_avr_bitrate * sec / 8);
    if(m_dataMode == AUDIO_LOCALFILE) return setFilePos(filepos);
//    if(m_streamType == ST_WEBFILE) return httpRange(m_lastHost, filepos);
    return false;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::setVolumeSteps(uint8_t steps) {
    m_vol_steps = steps;
    if(steps < 1) m_vol_steps = 64; /* avoid div-by-zero :-) */
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint8_t Audio::maxVolume() { return m_vol_steps; };
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::getTotalPlayingTime() {
    // Is set to zero by a connectToXXX() and starts as soon as the first audio data is available,
    // the time counting is not interrupted by a 'pause / resume'
    return millis() - m_PlayingStartTime;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::setTimeOffset(int sec) { // fast forward or rewind the current position in seconds
    if(!m_f_psramFound) {               log_w("PSRAM must be activated"); return false;} // guard
    if(m_dataMode != AUDIO_LOCALFILE /* && m_streamType == ST_WEBFILE */) return false;  // guard
#ifndef AUDIO_NO_SD_FS  
    if(m_dataMode == AUDIO_LOCALFILE && !m_audiofile)                       return false;  // guard
#endif    
    if(!m_avr_bitrate)                                                    return false;  // guard
    if(m_codec == CODEC_AAC) return false; // not impl. yet
    uint32_t oneSec = m_avr_bitrate / 8;                 // bytes decoded in one sec
    int32_t  offset = oneSec * sec;                      // bytes to be wind/rewind
    int32_t pos = getFilePos() - inBufferFilled();
    pos += offset;
    setFilePos(pos);

    return true;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::setFilePos(uint32_t pos) {
    if(!m_f_psramFound) {               log_w("PSRAM must be activated"); return false;} // guard
    if(m_dataMode != AUDIO_LOCALFILE /* && m_streamType == ST_WEBFILE */) return false;  // guard
#ifndef AUDIO_NO_SD_FS   
    if(m_dataMode == AUDIO_LOCALFILE && !m_audiofile)                       return false;  // guard
#endif    
    if(m_codec == CODEC_AAC)                                              return false;  // guard, not impl. yet

    uint32_t startAB = m_audioDataStart;                 // audioblock begin
    uint32_t endAB = m_audioDataStart + m_audioDataSize; // audioblock end
    if(pos < (int32_t)startAB) {pos = startAB;}
    if(pos >= (int32_t)endAB)  {pos = endAB;}


    m_validSamples = 0;
#ifndef AUDIO_NO_SD_FS   
    if(m_dataMode == AUDIO_LOCALFILE /* || m_streamType == ST_WEBFILE */) {
        m_resumeFilePos = pos;  // used in processLocalFile()
        return true;
    }
#endif
    return false;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::setSampleRate(uint32_t sampRate) {
    if(!sampRate) sampRate = 48000;
    if(sampRate < 8000 ) {
        AUDIO_INFO("Sample rate must not be smaller than 8kHz, found: %lu", sampRate);
        m_sampleRate = 8000;
    }
    m_sampleRate = sampRate;
    m_resampleRatio = (float)m_sampleRate / 48000.0f;
    return true;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::getSampleRate() { return m_sampleRate; }
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::setBitsPerSample(int bits) {
    if((bits != 16) && (bits != 8)) return false;
    m_bitsPerSample = bits;
    return true;
}
uint8_t Audio::getBitsPerSample() { return m_bitsPerSample; }
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::setChannels(int ch) {
    m_channels = ch;
    return true;
}
uint8_t Audio::getChannels() {
    if(m_channels == 0) { // this should not happen! #209
        m_channels = 2;
    }
    return m_channels;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::setBitrate(int br) {
    m_bitRate = br;
    if(br) return true;
    return false;
}
uint32_t Audio::getBitRate(bool avg) {
    if(avg) return m_avr_bitrate;
    return m_bitRate;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::setI2SCommFMT_LSB(bool commFMT) {
    // false: I2S communication format is by default I2S_COMM_FORMAT_I2S_MSB, right->left (AC101, PCM5102A)
    // true:  changed to I2S_COMM_FORMAT_I2S_LSB for some DACs (PT8211)
    //        Japanese or called LSBJ (Least Significant Bit Justified) format

    m_f_commFMT = commFMT;

    i2s_channel_disable(m_i2s_tx_handle);
    if(commFMT) {
        AUDIO_INFO("commFMT = LSBJ (Least Significant Bit Justified)");
        m_i2s_std_cfg.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    }
    else {
        AUDIO_INFO("commFMT = Philips");
        m_i2s_std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    }
    i2s_channel_reconfig_std_slot(m_i2s_tx_handle, &m_i2s_std_cfg.slot_cfg);
    i2s_channel_enable(m_i2s_tx_handle);
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::computeVUlevel(int16_t sample[2]) {
    static uint8_t sampleArray[2][4][8] = {0};
    static uint8_t cnt0 = 0, cnt1 = 0, cnt2 = 0, cnt3 = 0, cnt4 = 0;
    static bool    f_vu = false;

    auto avg = [&](uint8_t* sampArr) { // lambda, inner function, compute the average of 8 samples
        uint16_t av = 0;
        for(int i = 0; i < 8; i++) { av += sampArr[i]; }
        return av >> 3;
    };

    auto largest = [&](uint8_t* sampArr) { // lambda, inner function, compute the largest of 8 samples
        uint16_t maxValue = 0;
        for(int i = 0; i < 8; i++) {
            if(maxValue < sampArr[i]) maxValue = sampArr[i];
        }
        return maxValue;
    };

    if(cnt0 == 64) {
        cnt0 = 0;
        cnt1++;
    }
    if(cnt1 == 8) {
        cnt1 = 0;
        cnt2++;
    }
    if(cnt2 == 8) {
        cnt2 = 0;
        cnt3++;
    }
    if(cnt3 == 8) {
        cnt3 = 0;
        cnt4++;
        f_vu = true;
    }
    if(cnt4 == 8) { cnt4 = 0; }

    if(!cnt0) { // store every 64th sample in the array[0]
        sampleArray[LEFTCHANNEL][0][cnt1] = abs(sample[LEFTCHANNEL] >> 7);
        sampleArray[RIGHTCHANNEL][0][cnt1] = abs(sample[RIGHTCHANNEL] >> 7);
    }
    if(!cnt1) { // store argest from 64 * 8 samples in the array[1]
        sampleArray[LEFTCHANNEL][1][cnt2] = largest(sampleArray[LEFTCHANNEL][0]);
        sampleArray[RIGHTCHANNEL][1][cnt2] = largest(sampleArray[RIGHTCHANNEL][0]);
    }
    if(!cnt2) { // store avg from 64 * 8 * 8 samples in the array[2]
        sampleArray[LEFTCHANNEL][2][cnt3] = largest(sampleArray[LEFTCHANNEL][1]);
        sampleArray[RIGHTCHANNEL][2][cnt3] = largest(sampleArray[RIGHTCHANNEL][1]);
    }
    if(!cnt3) { // store avg from 64 * 8 * 8 * 8 samples in the array[3]
        sampleArray[LEFTCHANNEL][3][cnt4] = avg(sampleArray[LEFTCHANNEL][2]);
        sampleArray[RIGHTCHANNEL][3][cnt4] = avg(sampleArray[RIGHTCHANNEL][2]);
    }
    if(f_vu) {
        f_vu = false;
        m_vuLeft = avg(sampleArray[LEFTCHANNEL][3]);
        m_vuRight = avg(sampleArray[RIGHTCHANNEL][3]);
    }
    cnt1++;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint16_t Audio::getVUlevel() {
    // avg 0 ... 127
    if(!m_f_running) return 0;
    return (m_vuLeft << 8) + m_vuRight;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::setTone(int8_t gainLowPass, int8_t gainBandPass, int8_t gainHighPass) {
    // see https://www.earlevel.com/main/2013/10/13/biquad-calculator-v2/
    // values can be between -40 ... +6 (dB)

    m_gain0 = gainLowPass;
    m_gain1 = gainBandPass;
    m_gain2 = gainHighPass;

    // gain, attenuation (set in digital filters)
    int db = max(m_gain0, max(m_gain1, m_gain2));
    m_corr = pow10f((float)db / 20);

    IIR_calculateCoefficients(m_gain0, m_gain1, m_gain2);

    /*
          This will cause a clicking sound when adjusting the EQ.
          Because when the EQ is adjusted, the IIR filter will be cleared and played,
          mixed in the audio data frame, and a click-like sound will be produced.

          int16_t tmp[2]; tmp[0] = 0; tmp[1]= 0;

          IIR_filterChain0(tmp, true ); // flush the filter
          IIR_filterChain1(tmp, true ); // flush the filter
          IIR_filterChain2(tmp, true ); // flush the filter
        */
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::forceMono(bool m) { // #100 mono option
    m_f_forceMono = m;          // false stereo, true mono
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::setBalance(int8_t bal) { // bal -16...16
    if(bal < -16) bal = -16;
    if(bal > 16) bal = 16;
    m_balance = bal;

    computeLimit();
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::setVolume(uint8_t vol, uint8_t curve) { // curve 0: default, curve 1: flat at the beginning

    if(vol > m_vol_steps) m_vol = m_vol_steps;
    else m_vol = vol;

    if(curve > 1) m_curve = 1;
    else m_curve = curve;

    computeLimit();
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint8_t Audio::getVolume() { return m_vol; }
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint8_t Audio::getI2sPort() { return m_i2s_num; }
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::computeLimit() {    // is calculated when the volume or balance changes
    double l = 1, r = 1, v = 1; // assume 100%

    /* balance is left -16...+16 right */
    /* TODO: logarithmic scaling of balance, too? */
    if(m_balance > 0) { r -= (double)abs(m_balance) / 16; }
    else if(m_balance < 0) { l -= (double)abs(m_balance) / 16; }

    switch(m_curve) {
        case 0:
            v = (double)pow(m_vol, 2) / pow(m_vol_steps, 2); // square (default)
            break;
        case 1: // logarithmic
            double log1 = log(1);
            if(m_vol > 0) { v = m_vol * ((std::exp(log1 + (m_vol - 1) * (std::log(m_vol_steps) - log1) / (m_vol_steps - 1))) / m_vol_steps) / m_vol_steps; }
            else { v = 0; }
            break;
    }

    m_limit_left = l * v;
    m_limit_right = r * v;

    // log_i("m_limit_left %f,  m_limit_right %f ",m_limit_left, m_limit_right);
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::Gain(int16_t* sample) {
    /* important: these multiplications must all be signed ints, or the result will be invalid */
    sample[LEFTCHANNEL]  *= m_limit_left ;
    sample[RIGHTCHANNEL] *= m_limit_right;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::inBufferFilled() {
    // current audio input buffer fillsize in bytes
    return InBuff.bufferFilled();
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::inBufferFree() {
    // current audio input buffer free space in bytes
    return InBuff.freeSpace();
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::inBufferSize() {
    // current audio input buffer size in bytes
    return InBuff.getBufsize();
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::setBufferSize(size_t mbs) {
    // set audio input buffer size in bytes
    return InBuff.setBufsize(mbs);
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//            ***     D i g i t a l   b i q u a d r a t i c     f i l t e r     ***
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::IIR_calculateCoefficients(int8_t G0, int8_t G1, int8_t G2) { // Infinite Impulse Response (IIR) filters

    // G1 - gain low shelf   set between -40 ... +6 dB
    // G2 - gain peakEQ      set between -40 ... +6 dB
    // G3 - gain high shelf  set between -40 ... +6 dB
    // https://www.earlevel.com/main/2012/11/26/biquad-c-source-code/

    if(getSampleRate() < 1000) return; // fuse

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    if(G0 < -40) G0 = -40; // -40dB -> Vin*0.01
    if(G0 > 6) G0 = 6;     // +6dB -> Vin*2
    if(G1 < -40) G1 = -40;
    if(G1 > 6) G1 = 6;
    if(G2 < -40) G2 = -40;
    if(G2 > 6) G2 = 6;

    const float FcLS = 500;    // Frequency LowShelf[Hz]
    const float FcPKEQ = 3000; // Frequency PeakEQ[Hz]
    float       FcHS = 6000;   // Frequency HighShelf[Hz]

    if(getSampleRate() < FcHS * 2 - 100) { // Prevent HighShelf filter from clogging
        FcHS = getSampleRate() / 2 - 100;
        // according to the sampling theorem, the sample rate must be at least 2 * 6000 >= 12000Hz for a filter
        // frequency of 6000Hz. If this is not the case, the filter frequency (plus a reserve of 100Hz) is lowered
        AUDIO_INFO("Highshelf frequency lowered, from 6000Hz to %luHz", (long unsigned int)FcHS);
    }
    float K, norm, Q, Fc, V;

    // LOWSHELF
    Fc = (float)FcLS / (float)getSampleRate(); // Cutoff frequency
    K = tanf((float)PI * Fc);
    V = powf(10, fabs(G0) / 20.0);

    if(G0 >= 0) { // boost
        norm = 1 / (1 + sqrtf(2) * K + K * K);
        m_filter[LOWSHELF].a0 = (1 + sqrtf(2 * V) * K + V * K * K) * norm;
        m_filter[LOWSHELF].a1 = 2 * (V * K * K - 1) * norm;
        m_filter[LOWSHELF].a2 = (1 - sqrtf(2 * V) * K + V * K * K) * norm;
        m_filter[LOWSHELF].b1 = 2 * (K * K - 1) * norm;
        m_filter[LOWSHELF].b2 = (1 - sqrtf(2) * K + K * K) * norm;
    }
    else { // cut
        norm = 1 / (1 + sqrtf(2 * V) * K + V * K * K);
        m_filter[LOWSHELF].a0 = (1 + sqrtf(2) * K + K * K) * norm;
        m_filter[LOWSHELF].a1 = 2 * (K * K - 1) * norm;
        m_filter[LOWSHELF].a2 = (1 - sqrtf(2) * K + K * K) * norm;
        m_filter[LOWSHELF].b1 = 2 * (V * K * K - 1) * norm;
        m_filter[LOWSHELF].b2 = (1 - sqrtf(2 * V) * K + V * K * K) * norm;
    }

    // PEAK EQ
    Fc = (float)FcPKEQ / (float)getSampleRate(); // Cutoff frequency
    K = tanf((float)PI * Fc);
    V = powf(10, fabs(G1) / 20.0);
    Q = 2.5;      // Quality factor
    if(G1 >= 0) { // boost
        norm = 1 / (1 + 1 / Q * K + K * K);
        m_filter[PEAKEQ].a0 = (1 + V / Q * K + K * K) * norm;
        m_filter[PEAKEQ].a1 = 2 * (K * K - 1) * norm;
        m_filter[PEAKEQ].a2 = (1 - V / Q * K + K * K) * norm;
        m_filter[PEAKEQ].b1 = m_filter[PEAKEQ].a1;
        m_filter[PEAKEQ].b2 = (1 - 1 / Q * K + K * K) * norm;
    }
    else { // cut
        norm = 1 / (1 + V / Q * K + K * K);
        m_filter[PEAKEQ].a0 = (1 + 1 / Q * K + K * K) * norm;
        m_filter[PEAKEQ].a1 = 2 * (K * K - 1) * norm;
        m_filter[PEAKEQ].a2 = (1 - 1 / Q * K + K * K) * norm;
        m_filter[PEAKEQ].b1 = m_filter[PEAKEQ].a1;
        m_filter[PEAKEQ].b2 = (1 - V / Q * K + K * K) * norm;
    }

    // HIGHSHELF
    Fc = (float)FcHS / (float)getSampleRate(); // Cutoff frequency
    K = tanf((float)PI * Fc);
    V = powf(10, fabs(G2) / 20.0);
    if(G2 >= 0) { // boost
        norm = 1 / (1 + sqrtf(2) * K + K * K);
        m_filter[HIFGSHELF].a0 = (V + sqrtf(2 * V) * K + K * K) * norm;
        m_filter[HIFGSHELF].a1 = 2 * (K * K - V) * norm;
        m_filter[HIFGSHELF].a2 = (V - sqrtf(2 * V) * K + K * K) * norm;
        m_filter[HIFGSHELF].b1 = 2 * (K * K - 1) * norm;
        m_filter[HIFGSHELF].b2 = (1 - sqrtf(2) * K + K * K) * norm;
    }
    else {
        norm = 1 / (V + sqrtf(2 * V) * K + K * K);
        m_filter[HIFGSHELF].a0 = (1 + sqrtf(2) * K + K * K) * norm;
        m_filter[HIFGSHELF].a1 = 2 * (K * K - 1) * norm;
        m_filter[HIFGSHELF].a2 = (1 - sqrtf(2) * K + K * K) * norm;
        m_filter[HIFGSHELF].b1 = 2 * (K * K - V) * norm;
        m_filter[HIFGSHELF].b2 = (V - sqrtf(2 * V) * K + K * K) * norm;
    }

    //    log_i("LS a0=%f, a1=%f, a2=%f, b1=%f, b2=%f", m_filter[0].a0, m_filter[0].a1, m_filter[0].a2,
    //                                                  m_filter[0].b1, m_filter[0].b2);
    //    log_i("EQ a0=%f, a1=%f, a2=%f, b1=%f, b2=%f", m_filter[1].a0, m_filter[1].a1, m_filter[1].a2,
    //                                                  m_filter[1].b1, m_filter[1].b2);
    //    log_i("HS a0=%f, a1=%f, a2=%f, b1=%f, b2=%f", m_filter[2].a0, m_filter[2].a1, m_filter[2].a2,
    //                                                  m_filter[2].b1, m_filter[2].b2);
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// clang-format off
void Audio::IIR_filterChain0(int16_t iir_in[2], bool clear) { // Infinite Impulse Response (IIR) filters

    uint8_t z1 = 0, z2 = 1;
    enum : uint8_t { in = 0, out = 1 };
    float          inSample[2];
    float          outSample[2];
    static int16_t iir_out[2];

    if(clear) {
        memset(m_filterBuff, 0, sizeof(m_filterBuff)); // zero IIR filterbuffer
        iir_out[0] = 0;
        iir_out[1] = 0;
        iir_in[0] = 0;
        iir_in[1] = 0;
    }

    inSample[LEFTCHANNEL] = (float)(iir_in[LEFTCHANNEL]);
    inSample[RIGHTCHANNEL] = (float)(iir_in[RIGHTCHANNEL]);

    outSample[LEFTCHANNEL] =
        m_filter[0].a0 * inSample[LEFTCHANNEL] + m_filter[0].a1 * m_filterBuff[0][z1][in][LEFTCHANNEL] +
        m_filter[0].a2 * m_filterBuff[0][z2][in][LEFTCHANNEL] - m_filter[0].b1 * m_filterBuff[0][z1][out][LEFTCHANNEL] -
        m_filter[0].b2 * m_filterBuff[0][z2][out][LEFTCHANNEL];

    m_filterBuff[0][z2][in][LEFTCHANNEL] = m_filterBuff[0][z1][in][LEFTCHANNEL];
    m_filterBuff[0][z1][in][LEFTCHANNEL] = inSample[LEFTCHANNEL];
    m_filterBuff[0][z2][out][LEFTCHANNEL] = m_filterBuff[0][z1][out][LEFTCHANNEL];
    m_filterBuff[0][z1][out][LEFTCHANNEL] = outSample[LEFTCHANNEL];
    iir_out[LEFTCHANNEL] = (int16_t)outSample[LEFTCHANNEL];

    outSample[RIGHTCHANNEL] = m_filter[0].a0 * inSample[RIGHTCHANNEL] +
                              m_filter[0].a1 * m_filterBuff[0][z1][in][RIGHTCHANNEL] +
                              m_filter[0].a2 * m_filterBuff[0][z2][in][RIGHTCHANNEL] -
                              m_filter[0].b1 * m_filterBuff[0][z1][out][RIGHTCHANNEL] -
                              m_filter[0].b2 * m_filterBuff[0][z2][out][RIGHTCHANNEL];

    m_filterBuff[0][z2][in][RIGHTCHANNEL] = m_filterBuff[0][z1][in][RIGHTCHANNEL];
    m_filterBuff[0][z1][in][RIGHTCHANNEL] = inSample[RIGHTCHANNEL];
    m_filterBuff[0][z2][out][RIGHTCHANNEL] = m_filterBuff[0][z1][out][RIGHTCHANNEL];
    m_filterBuff[0][z1][out][RIGHTCHANNEL] = outSample[RIGHTCHANNEL];
    iir_out[RIGHTCHANNEL] = (int16_t)outSample[RIGHTCHANNEL];

    iir_in[LEFTCHANNEL] = iir_out[LEFTCHANNEL];
    iir_in[RIGHTCHANNEL] = iir_out[RIGHTCHANNEL];
    return;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::IIR_filterChain1(int16_t iir_in[2], bool clear) { // Infinite Impulse Response (IIR) filters

    uint8_t z1 = 0, z2 = 1;
    enum : uint8_t { in = 0, out = 1 };
    float          inSample[2];
    float          outSample[2];
    static int16_t iir_out[2];

    if(clear) {
        memset(m_filterBuff, 0, sizeof(m_filterBuff)); // zero IIR filterbuffer
        iir_out[0] = 0;
        iir_out[1] = 0;
        iir_in[0] = 0;
        iir_in[1] = 0;
    }

    inSample[LEFTCHANNEL] = (float)(iir_in[LEFTCHANNEL]);
    inSample[RIGHTCHANNEL] = (float)(iir_in[RIGHTCHANNEL]);

    outSample[LEFTCHANNEL] =
        m_filter[1].a0 * inSample[LEFTCHANNEL] + m_filter[1].a1 * m_filterBuff[1][z1][in][LEFTCHANNEL] +
        m_filter[1].a2 * m_filterBuff[1][z2][in][LEFTCHANNEL] - m_filter[1].b1 * m_filterBuff[1][z1][out][LEFTCHANNEL] -
        m_filter[1].b2 * m_filterBuff[1][z2][out][LEFTCHANNEL];

    m_filterBuff[1][z2][in][LEFTCHANNEL] = m_filterBuff[1][z1][in][LEFTCHANNEL];
    m_filterBuff[1][z1][in][LEFTCHANNEL] = inSample[LEFTCHANNEL];
    m_filterBuff[1][z2][out][LEFTCHANNEL] = m_filterBuff[1][z1][out][LEFTCHANNEL];
    m_filterBuff[1][z1][out][LEFTCHANNEL] = outSample[LEFTCHANNEL];
    iir_out[LEFTCHANNEL] = (int16_t)outSample[LEFTCHANNEL];

    outSample[RIGHTCHANNEL] = m_filter[1].a0 * inSample[RIGHTCHANNEL] +
                              m_filter[1].a1 * m_filterBuff[1][z1][in][RIGHTCHANNEL] +
                              m_filter[1].a2 * m_filterBuff[1][z2][in][RIGHTCHANNEL] -
                              m_filter[1].b1 * m_filterBuff[1][z1][out][RIGHTCHANNEL] -
                              m_filter[1].b2 * m_filterBuff[1][z2][out][RIGHTCHANNEL];

    m_filterBuff[1][z2][in][RIGHTCHANNEL] = m_filterBuff[1][z1][in][RIGHTCHANNEL];
    m_filterBuff[1][z1][in][RIGHTCHANNEL] = inSample[RIGHTCHANNEL];
    m_filterBuff[1][z2][out][RIGHTCHANNEL] = m_filterBuff[1][z1][out][RIGHTCHANNEL];
    m_filterBuff[1][z1][out][RIGHTCHANNEL] = outSample[RIGHTCHANNEL];
    iir_out[RIGHTCHANNEL] = (int16_t)outSample[RIGHTCHANNEL];

    iir_in[LEFTCHANNEL] = iir_out[LEFTCHANNEL];
    iir_in[RIGHTCHANNEL] = iir_out[RIGHTCHANNEL];
    return;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::IIR_filterChain2(int16_t iir_in[2], bool clear) { // Infinite Impulse Response (IIR) filters

    uint8_t z1 = 0, z2 = 1;
    enum : uint8_t { in = 0, out = 1 };
    float          inSample[2];
    float          outSample[2];
    static int16_t iir_out[2];

    if(clear) {
        memset(m_filterBuff, 0, sizeof(m_filterBuff)); // zero IIR filterbuffer
        iir_out[0] = 0;
        iir_out[1] = 0;
        iir_in[0] = 0;
        iir_in[1] = 0;
    }

    inSample[LEFTCHANNEL] = (float)(iir_in[LEFTCHANNEL]);
    inSample[RIGHTCHANNEL] = (float)(iir_in[RIGHTCHANNEL]);

    outSample[LEFTCHANNEL] =
        m_filter[2].a0 * inSample[LEFTCHANNEL] + m_filter[2].a1 * m_filterBuff[2][z1][in][LEFTCHANNEL] +
        m_filter[2].a2 * m_filterBuff[2][z2][in][LEFTCHANNEL] - m_filter[2].b1 * m_filterBuff[2][z1][out][LEFTCHANNEL] -
        m_filter[2].b2 * m_filterBuff[2][z2][out][LEFTCHANNEL];

    m_filterBuff[2][z2][in][LEFTCHANNEL] = m_filterBuff[2][z1][in][LEFTCHANNEL];
    m_filterBuff[2][z1][in][LEFTCHANNEL] = inSample[LEFTCHANNEL];
    m_filterBuff[2][z2][out][LEFTCHANNEL] = m_filterBuff[2][z1][out][LEFTCHANNEL];
    m_filterBuff[2][z1][out][LEFTCHANNEL] = outSample[LEFTCHANNEL];
    iir_out[LEFTCHANNEL] = (int16_t)outSample[LEFTCHANNEL];

    outSample[RIGHTCHANNEL] = m_filter[2].a0 * inSample[RIGHTCHANNEL] +
                              m_filter[2].a1 * m_filterBuff[2][z1][in][RIGHTCHANNEL] +
                              m_filter[2].a2 * m_filterBuff[2][z2][in][RIGHTCHANNEL] -
                              m_filter[2].b1 * m_filterBuff[2][z1][out][RIGHTCHANNEL] -
                              m_filter[2].b2 * m_filterBuff[2][z2][out][RIGHTCHANNEL];

    m_filterBuff[2][z2][in][RIGHTCHANNEL] = m_filterBuff[2][z1][in][RIGHTCHANNEL];
    m_filterBuff[2][z1][in][RIGHTCHANNEL] = inSample[RIGHTCHANNEL];
    m_filterBuff[2][z2][out][RIGHTCHANNEL] = m_filterBuff[2][z1][out][RIGHTCHANNEL];
    m_filterBuff[2][z1][out][RIGHTCHANNEL] = outSample[RIGHTCHANNEL];
    iir_out[RIGHTCHANNEL] = (int16_t)outSample[RIGHTCHANNEL];

    iir_in[LEFTCHANNEL] = iir_out[LEFTCHANNEL];
    iir_in[RIGHTCHANNEL] = iir_out[RIGHTCHANNEL];
    return;
}
// clang-format on
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//    AAC - T R A N S P O R T S T R E A M
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::ts_parsePacket(uint8_t* packet, uint8_t* packetStart, uint8_t* packetLength) {

    bool log = false;

    const uint8_t TS_PACKET_SIZE = 188;
    const uint8_t PAYLOAD_SIZE = 184;
    const uint8_t PID_ARRAY_LEN = 4;

    (void)PAYLOAD_SIZE; // suppress [-Wunused-variable]

    typedef struct {
        int number = 0;
        int pids[PID_ARRAY_LEN];
    } pid_array;

    static pid_array pidsOfPMT;
    static int       PES_DataLength = 0;
    static int       pidOfAAC = 0;

    if(packet == NULL) {
        if(log) log_w("parseTS reset");
        for(int i = 0; i < PID_ARRAY_LEN; i++) pidsOfPMT.pids[i] = 0;
        PES_DataLength = 0;
        pidOfAAC = 0;
        return true;
    }

    // --------------------------------------------------------------------------------------------------------
    // 0. Byte SyncByte  | 0 | 1 | 0 | 0 | 0 | 1 | 1 | 1 | always bit pattern of 0x47
    //---------------------------------------------------------------------------------------------------------
    // 1. Byte           |PUSI|TP|   |PID|PID|PID|PID|PID|
    //---------------------------------------------------------------------------------------------------------
    // 2. Byte           |PID|PID|PID|PID|PID|PID|PID|PID|
    //---------------------------------------------------------------------------------------------------------
    // 3. Byte           |TSC|TSC|AFC|AFC|CC |CC |CC |CC |
    //---------------------------------------------------------------------------------------------------------
    // 4.-187. Byte      |Payload data if AFC==01 or 11  |
    //---------------------------------------------------------------------------------------------------------

    // PUSI Payload unit start indicator, set when this packet contains the first byte of a new payload unit.
    //      The first byte of the payload will indicate where this new payload unit starts.
    // TP   Transport priority, set when the current packet has a higher priority than other packets with the same PID.
    // PID  Packet Identifier, describing the payload data.
    // TSC  Transport scrambling control, '00' = Not scrambled.
    // AFC  Adaptation field control, 01 – no adaptation field, payload only, 10 – adaptation field only, no payload,
    //                                11 – adaptation field followed by payload, 00 – RESERVED for future use
    // CC   Continuity counter, Sequence number of payload packets (0x00 to 0x0F) within each stream (except PID 8191)

    // for(int i = 1; i < 188; i++) {printf("%02X ", packet[i - 1]); if(i && (i % 16 == 0)) printf("\n");}
    // printf("\n----------\n");

    if(packet[0] != 0x47) {
        log_e("ts SyncByte not found, first bytes are 0x%02X 0x%02X 0x%02X 0x%02X", packet[0], packet[1], packet[2], packet[3]);
        // stopSong();
        return false;
    }
    int PID = (packet[1] & 0x1F) << 8 | (packet[2] & 0xFF);
    if(log) log_w("PID: 0x%04X(%d)", PID, PID);
    int PUSI = (packet[1] & 0x40) >> 6;
    if(log) log_w("Payload Unit Start Indicator: %d", PUSI);
    int AFC = (packet[3] & 0x30) >> 4;
    if(log) log_w("Adaption Field Control: %d", AFC);

    int AFL = -1;
    if((AFC & 0b10) == 0b10) {  // AFC '11' Adaptation Field followed
        AFL = packet[4] & 0xFF; // Adaptation Field Length
        if(log) log_w("Adaptation Field Length: %d", AFL);
    }
    int PLS = PUSI ? 5 : 4;     // PayLoadStart, Payload Unit Start Indicator
    if(AFL > 0) PLS += AFL + 1; // skip adaption field

    if(AFC == 2){ // The TS package contains only an adaptation Field and no user data.
        *packetStart = AFL + 1;
        *packetLength = 0;
        return true;
    }

    if(PID == 0) {
        // Program Association Table (PAT) - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        if(log) log_w("PAT");
        pidsOfPMT.number = 0;
        pidOfAAC = 0;

        int startOfProgramNums = 8;
        int lengthOfPATValue = 4;
        int sectionLength = ((packet[PLS + 1] & 0x0F) << 8) | (packet[PLS + 2] & 0xFF);
        if(log) log_w("Section Length: %d", sectionLength);
        int program_number, program_map_PID;
        int indexOfPids = 0;
        (void)program_number; // [-Wunused-but-set-variable]
        for(int i = startOfProgramNums; i <= sectionLength; i += lengthOfPATValue) {
            program_number = ((packet[PLS + i] & 0xFF) << 8) | (packet[PLS + i + 1] & 0xFF);
            program_map_PID = ((packet[PLS + i + 2] & 0x1F) << 8) | (packet[PLS + i + 3] & 0xFF);
            if(log) log_w("Program Num: 0x%04X(%d) PMT PID: 0x%04X(%d)", program_number, program_number, program_map_PID, program_map_PID);
            pidsOfPMT.pids[indexOfPids++] = program_map_PID;
        }
        pidsOfPMT.number = indexOfPids;
        *packetStart = 0;
        *packetLength = 0;
        return true;
    }
    else if(PID == pidOfAAC) {
        static uint8_t fillData = 0;
        if(log) log_w("AAC");
        uint8_t posOfPacketStart = 4;
        if(AFL >= 0) {
            posOfPacketStart = 5 + AFL;
            if(log) log_w("posOfPacketStart: %d", posOfPacketStart);
        }
        // Packetized Elementary Stream (PES) - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        if(log) log_w("PES_DataLength %i", PES_DataLength);
        if(PES_DataLength > 0) {
            *packetStart = posOfPacketStart + fillData;
            *packetLength = TS_PACKET_SIZE - posOfPacketStart - fillData;
            if(log) log_w("packetlength %i", *packetLength);
            fillData = 0;
            PES_DataLength -= (*packetLength);
            return true;
        }
        else {
            int firstByte = packet[posOfPacketStart] & 0xFF;
            int secondByte = packet[posOfPacketStart + 1] & 0xFF;
            int thirdByte = packet[posOfPacketStart + 2] & 0xFF;
            if(log) log_w ("First 3 bytes: 0x%02X, 0x%02X, 0x%02X", firstByte, secondByte, thirdByte);
            if(firstByte == 0x00 && secondByte == 0x00 && thirdByte == 0x01) { // Packet start code prefix
                // --------------------------------------------------------------------------------------------------------
                // posOfPacketStart + 0...2     0x00, 0x00, 0x01                                          PES-Startcode
                //---------------------------------------------------------------------------------------------------------
                // posOfPacketStart + 3         0xE0 (Video) od 0xC0 (Audio)                              StreamID
                //---------------------------------------------------------------------------------------------------------
                // posOfPacketStart + 4...5     0xLL, 0xLL                                                PES Packet length
                //---------------------------------------------------------------------------------------------------------
                // posOfPacketStart + 6...7                                                               PTS/DTS Flags
                //---------------------------------------------------------------------------------------------------------
                // posOfPacketStart + 8         0xXX                                                      header length
                //---------------------------------------------------------------------------------------------------------

                uint8_t StreamID = packet[posOfPacketStart + 3] & 0xFF;
                if(StreamID >= 0xC0 && StreamID <= 0xDF) { ; } // okay ist audio stream
                if(StreamID >= 0xE0 && StreamID <= 0xEF) {log_e("video stream!"); return false; }
                int PES_PacketLength = ((packet[posOfPacketStart + 4] & 0xFF) << 8) + (packet[posOfPacketStart + 5] & 0xFF);
                if(log) log_w("PES PacketLength: %d", PES_PacketLength);
                bool PTS_flag = false;
                bool DTS_flag = false;
                int flag_byte1 = packet[posOfPacketStart + 6] & 0xFF;
                int flag_byte2 = packet[posOfPacketStart + 7] & 0xFF;  (void) flag_byte2; // unused yet
                if(flag_byte1 & 0b10000000) PTS_flag = true;
                if(flag_byte1 & 0b00000100) DTS_flag = true;
                if(log && PTS_flag) log_w("PTS_flag is set");
                if(log && DTS_flag) log_w("DTS_flag is set");
                uint8_t PES_HeaderDataLength = packet[posOfPacketStart + 8] & 0xFF;
                if(log) log_w("PES_headerDataLength %d", PES_HeaderDataLength);

                PES_DataLength = PES_PacketLength;
                int startOfData = PES_HeaderDataLength + 9;
                if(posOfPacketStart + startOfData >= 188) { // only fillers in packet
                    if(log) log_w("posOfPacketStart + startOfData %i", posOfPacketStart + startOfData);
                    *packetStart = 0;
                    *packetLength = 0;
                    PES_DataLength -= (PES_HeaderDataLength + 3);
                    fillData = (posOfPacketStart + startOfData) - 188;
                    if(log) log_w("fillData %i", fillData);
                    return true;
                }
                if(log) log_w("First AAC data byte: %02X", packet[posOfPacketStart + startOfData]);
                if(log) log_w("Second AAC data byte: %02X", packet[posOfPacketStart + startOfData + 1]);
                *packetStart = posOfPacketStart + startOfData;
                *packetLength = TS_PACKET_SIZE - posOfPacketStart - startOfData;
                PES_DataLength -= (*packetLength);
                PES_DataLength -= (PES_HeaderDataLength + 3);
                return true;
            }
            if(firstByte == 0 && secondByte == 0 && thirdByte == 0){
                // PES packet startcode prefix is 0x000000
                // skip such packets
                return true;
            }
        }
        *packetStart = 0;
        *packetLength = 0;
        log_e("PES not found");
        return false;
    }
    else if(pidsOfPMT.number) {
        //  Program Map Table (PMT) - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        for(int i = 0; i < pidsOfPMT.number; i++) {
            if(PID == pidsOfPMT.pids[i]) {
                if(log) log_w("PMT");
                int staticLengthOfPMT = 12;
                int sectionLength = ((packet[PLS + 1] & 0x0F) << 8) | (packet[PLS + 2] & 0xFF);
                if(log) log_w("Section Length: %d", sectionLength);
                int programInfoLength = ((packet[PLS + 10] & 0x0F) << 8) | (packet[PLS + 11] & 0xFF);
                if(log) log_w("Program Info Length: %d", programInfoLength);
                int cursor = staticLengthOfPMT + programInfoLength;
                while(cursor < sectionLength - 1) {
                    int streamType = packet[PLS + cursor] & 0xFF;
                    int elementaryPID = ((packet[PLS + cursor + 1] & 0x1F) << 8) | (packet[PLS + cursor + 2] & 0xFF);
                    if(log) log_w("Stream Type: 0x%02X Elementary PID: 0x%04X", streamType, elementaryPID);

                    if(streamType == 0x0F || streamType == 0x11 || streamType == 0x04) {
                        if(log) log_w("AAC PID discover");
                        pidOfAAC = elementaryPID;
                    }
                    int esInfoLength = ((packet[PLS + cursor + 3] & 0x0F) << 8) | (packet[PLS + cursor + 4] & 0xFF);
                    if(log) log_w("ES Info Length: 0x%04X", esInfoLength);
                    cursor += 5 + esInfoLength;
                }
            }
        }
        *packetStart = 0;
        *packetLength = 0;
        return true;
    }
    // PES received before PAT and PMT seen
    *packetStart = 0;
    *packetLength = 0;
    return false;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//    W E B S T R E A M  -  H E L P   F U N C T I O N S
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint16_t Audio::readMetadata(uint16_t maxBytes, bool first) {
    static uint16_t pos_ml = 0; // determines the current position in metaline
    static uint16_t metalen = 0;
    uint16_t        res = 0;
    ps_ptr<char>buff; buff.alloc(4096); buff.clear(); // is max 256 *16
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(first) {
        pos_ml = 0;
        metalen = 0;
        return 0;
    }
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    if(!maxBytes) return 0; // guard

    if(!metalen) {
        int b = _client->read(); // First byte of metadata?
        if (b < 0) {
            AUDIO_INFO("client->read() failed (%d)", b);
            return 0;
        }
        metalen = b * 16;        // New count for metadata including length byte, max 4096
        pos_ml = 0;
        buff[pos_ml] = 0; // Prepare for new line
        res = 1;
    }
    if(!metalen) {
        m_metacount = m_metaint;
        return res;
    } // metalen is 0
    uint16_t a = _client->readBytes(&buff[pos_ml], min((uint16_t)(metalen - pos_ml), (uint16_t)(maxBytes)));
    res += a;
    pos_ml += a;

    if(pos_ml == metalen) {
        buff[pos_ml] = '\0';
        if(buff.strlen() > 0) { // Any info present?
            // metaline contains artist and song name.  For example:
            // "StreamTitle='Don McLean - American Pie';StreamUrl='';"
            // Sometimes it is just other info like:
            // "StreamTitle='60s 03 05 Magic60s';StreamUrl='';"
            // Isolate the StreamTitle, remove leading and trailing quotes if present.
            latinToUTF8(buff);          // convert to UTF-8 if necessary
            int pos = buff.index_of_icase("song_spot", 0); // remove some irrelevant infos
            if(pos > 3) {                               // e.g. song_spot="T" MediaBaseId="0" itunesTrackId="0"
                buff[pos] = 0;
            }
            showstreamtitle(buff.get()); // Show artist and title if present in metadata
        }
        m_metacount = m_metaint;
        metalen = 0;
        pos_ml = 0;
    }
    return res;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
size_t Audio::readChunkSize(uint8_t* bytes) {
    uint8_t  byteCounter = 0;
    size_t   chunksize = 0;
    // bool     parsingChunkSize = true;
    int      b = 0;
    std::string chunkLine;
    uint32_t ctime = millis();
    uint32_t timeout = 2000; // ms

    while (true) {
        if ((millis() - ctime) > timeout) {
            log_e("chunkedDataTransfer: timeout");
            stopSong();
            return 0;
        }
        if(!_client->available()) continue; // no data available yet
        b = _client->read();
        if (b < 0) continue; // -1 = no data

        byteCounter++;
        if (b == '\n') break; // End of chunk-size line
        if (b == '\r') continue;

        chunkLine += static_cast<char>(b);
    }

    // chunkLine z.B.: "2A", oder "2A;foo=bar"
    size_t semicolonPos = chunkLine.find(';');
    std::string hexSize = (semicolonPos != std::string::npos) ? chunkLine.substr(0, semicolonPos) : chunkLine;

    // Converted hex number
    chunksize = strtoul(hexSize.c_str(), nullptr, 16);
    *bytes = byteCounter;

    if (chunksize == 0) { // Special case: Last chunk recognized (0) => Next read and reject "\ r \ n"
        // Reading to complete "\ r \ n" was received
        uint8_t crlf[2] = {0}; (void)crlf; // suppress [-Wunused-variable]
        uint8_t idx = 0;
        ctime = millis();
        while (idx < 2 && (millis() - ctime) < timeout) {
            int ch = _client->read();
            if (ch < 0) continue;
            crlf[idx++] = static_cast<uint8_t>(ch);
        }
    }

    return chunksize;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool Audio::readID3V1Tag() {
    if (m_codec != CODEC_MP3) return false;
    ps_ptr<char>chBuff;
    chBuff.alloc(256);

    const uint8_t* p = InBuff.getReadPtr();

    // Lambda for simplification
    auto readID3Field = [&](ps_ptr<char>& field, const uint8_t* src, size_t len, const char* label = nullptr) {
        field.alloc(len + 1);
        memcpy(field.get(), src, len);
        field[len] = '\0';
        latinToUTF8(field);
        if (field.strlen() > 0 && label && audio_id3data) {
            field.insert(label, 0);
            audio_id3data(field.get());
        }
    };

    if (InBuff.bufferFilled() == 128 && startsWith((const char*)p, "TAG")) {
        ps_ptr<char> title, artist, album, year, comment;

        readID3Field(title,   p + 3,     30, "Title: ");
        readID3Field(artist,  p + 33,    30, "Artist: ");
        readID3Field(album,   p + 63,    30, "Album: ");
        readID3Field(year,    p + 93,    4,  "Year: ");
        readID3Field(comment, p + 97,    30, "Comment: ");

        uint8_t zeroByte = p[125];
        uint8_t track = p[126];
        uint8_t genre8 = p[127];

        AUDIO_INFO(zeroByte ? "ID3 version: 1" : "ID3 Version 1.1");

        if (zeroByte == 0) {
            sprintf(chBuff.get(), "Track Number: %d", track);
            if (audio_id3data) audio_id3data(chBuff.get());
        }

        if (genre8 < 192) {
            sprintf(chBuff.get(), "Genre: %d", genre8);
            if (audio_id3data) audio_id3data(chBuff.get());
        }

        return true;
    }

    if (InBuff.bufferFilled() == 227 && startsWith((const char*)p, "TAG+")) { // "TAG+" "can exist as an extension, does not overwrite" TAG"
        AUDIO_INFO("ID3 version: 1 - Enhanced TAG");

        ps_ptr<char> title, artist, album, genre;

        readID3Field(title,  p + 4,     60, "Title: ");
        readID3Field(artist, p + 64,    60, "Artist: ");
        readID3Field(album,  p + 124,   60, "Album: ");
        readID3Field(genre,  p + 185,   30, "Genre: ");

        // optional expansion: speed, start-time, end-time
        return true;
    }

    return false;
    // [1] https://en.wikipedia.org/wiki/List_of_ID3v1_Genres
    // [2] https://en.wikipedia.org/wiki/ID3#ID3v1_and_ID3v1.1[5]
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

boolean Audio::streamDetection(uint32_t bytesAvail) {
    if(!m_lastHost.valid()) {log_e("m_lastHost is empty"); return false;}
    static uint32_t tmr_slow = millis();
    static uint32_t tmr_lost = millis();
    static uint8_t  cnt_slow = 0;
    static uint8_t  cnt_lost = 0;

    // if within one second the content of the audio buffer falls below the size of an audio frame 100 times,
    // issue a message
    if(tmr_slow + 1000 < millis()) {
        tmr_slow = millis();
        if(cnt_slow > 100) AUDIO_INFO("slow stream, dropouts are possible");
        cnt_slow = 0;
    }
    if(InBuff.bufferFilled() < InBuff.getMaxBlockSize()) cnt_slow++;
    if(bytesAvail) {
        tmr_lost = millis() + 1000;
        cnt_lost = 0;
    }
    if(InBuff.bufferFilled() > InBuff.getMaxBlockSize() * 2) return false; // enough data available to play

    // if no audio data is received within three seconds, a new connection attempt is started.
    if(tmr_lost < millis()) {
        cnt_lost++;
        tmr_lost = millis() + 1000;
        if(cnt_lost == 5) { // 5s no data?
            cnt_lost = 0;
            AUDIO_INFO("Stream lost -> try new connection");
            m_f_reset_m3u8Codec = false;
            connecttohost(m_lastHost.get());
            return true;
        }
    }
    return false;
}
#ifndef AUDIO_NO_SD_FS	
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::find_m4a_atom(uint32_t fileSize, const char* atomName, uint32_t depth) {

    while (m_audiofile.position() < fileSize) {
        uint32_t atomStart = m_audiofile.position(); // Position of the current atom
        uint32_t atomSize;
        char atomType[5] = {0};
        m_audiofile.read((uint8_t*)&atomSize, 4);    // Read the atom size (4 bytes) and the atom type (4 bytes)
        if(!atomSize) m_audiofile.read((uint8_t*)&atomSize, 4); // skip 4 byte offset field
        m_audiofile.read((uint8_t*)atomType, 4);

        atomSize = bswap32(atomSize);              // Convert atom size from big-endian to little-endian
        ///    log_w("%*sAtom '%s' found at position %u with size %u bytes", depth * 2, "", atomType, atomStart, atomSize);
        if (strncmp(atomType, atomName, 4) == 0) return atomStart;

        if (atomSize == 1) {                       // If the atom has a size of 1, an 'Extended Size' is used
            uint64_t extendedSize;
            m_audiofile.read((uint8_t*)&extendedSize, 8);
            extendedSize = bswap64(extendedSize);
        //    log_w("%*sExtended size: %llu bytes\n", depth * 2, "", extendedSize);
            atomSize = (uint32_t)extendedSize;     // Limit to uint32_t for further processing
        }

        // If the atom is a container, read the atoms contained in it recursively
        if (strncmp(atomType, "moov", 4) == 0 || strncmp(atomType, "trak", 4) == 0 ||
            strncmp(atomType, "mdia", 4) == 0 || strncmp(atomType, "minf", 4) == 0 ||
            strncmp(atomType, "stbl", 4) == 0 || strncmp(atomType, "meta", 4) == 0 ||
            strncmp(atomType, "udta", 4) == 0 )  {
            // Go recursively into the atom, read the contents
            uint32_t pos = find_m4a_atom(atomStart + atomSize,  atomName, depth + 1);
            if(pos) return pos;
        } else {
            m_audiofile.seek(atomStart + atomSize);    // No container atom, jump to the next atom
        }
    }
    return 0;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::seek_m4a_ilst() {    // ilist - item list atom, contains the metadata
    if(!m_audiofile) return; // guard
    m_audiofile.seek(0);
    uint32_t fileSize = m_audiofile.size();
    const char atomName[] = "ilst";
    uint32_t atomStart = find_m4a_atom(fileSize, atomName);
    if(!atomStart) {
        AUDIO_INFO("ma4 atom ilst not found");
        m_audiofile.seek(0);
        return;
    }

    uint32_t   seekpos = atomStart;
    const char info[12][6] = {"nam\0", "ART\0", "alb\0", "too\0", "cmt\0", "wrt\0", "tmpo\0", "trkn\0", "day\0", "cpil\0", "aART\0", "gen\0"};

    seekpos = atomStart;
    char buff[4];
    uint32_t len = 0;
    m_audiofile.seek(seekpos);
    m_audiofile.readBytes(buff, 4);
    len = bigEndian((uint8_t*)buff, 4);
    if(!len) {
        m_audiofile.readBytes(buff, 4); // 4bytes offset filed
        len = bigEndian((uint8_t*)buff, 4) + 16;
    }
    if(len > 1024) len = 1024;
    //    log_w("found at pos %i, len %i", seekpos, len);

    ps_ptr<uint8_t> data;
    data.alloc(len, "data");
    data.clear();
    len -= 4;
    m_audiofile.seek(seekpos);
    m_audiofile.read(data.get(), len);

    int offset = 0;
    for(int i = 0; i < 12; i++) {
        offset = specialIndexOf(data.get(), info[i], len, true); // seek info[] with '\0'
        if(offset > 0) {
            offset += 19;
            if(*(data.get() + offset) == 0) offset++;
            char   value[256] = {0};
            size_t temp = strlen((const char*)data.get() + offset);
            if(temp > 254) temp = 254;
            memcpy(value, (data.get() + offset), temp);
            value[temp] = '\0';
            ps_ptr<char>chBuf = {};
            if(i == 0)  chBuf.appendf("Title: %s", value);
            if(i == 1)  chBuf.appendf("Artist: %s", value);
            if(i == 2)  chBuf.appendf("Album: %s", value);
            if(i == 3)  chBuf.appendf("Encoder: %s", value);
            if(i == 4)  chBuf.appendf("Comment: %s", value);
            if(i == 5)  chBuf.appendf("Composer: %s", value);
            if(i == 6)  chBuf.appendf("BPM: %s", value);
            if(i == 7)  chBuf.appendf("Track Number: %s", value);
            if(i == 8)  chBuf.appendf("Year: %s", value);
            if(i == 9)  chBuf.appendf("Compile: %s", value);
            if(i == 10) chBuf.appendf("Album Artist: %s", value);
            if(i == 11) chBuf.appendf("Types of: %s", value);
            if(chBuf.valid()) {
                if(audio_id3data) audio_id3data(chBuf.get());
            }
        }
    }
    m_f_m4aID3dataAreRead = true;
    m_audiofile.seek(0);
    return;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void Audio::seek_m4a_stsz() {
    // stsz says what size each sample is in bytes. This is important for the decoder to be able to start at a chunk,
    // and then go through each sample by its size. The stsz atom can be behind the audio block. Therefore, searching
    // for the stsz atom is only applicable to local files.

    /* atom hierarchy (example)_________________________________________________________________________________________

      ftyp -> moov -> trak -> tkhd
              free    udta    mdia -> mdhd
              mdat                    hdlr
              mvhd                    minf -> smhd
                                              dinf
                                              stbl -> stsd
                                                      stts
                                                      stsc
                                                      stsz -> determine and return the position and number of entries
                                                      stco
      __________________________________________________________________________________________________________________*/

    struct m4a_Atom {
        int  pos;
        int  size;
        char name[5] = {0};
    } atom, at, tmp;

    // c99 has no inner functions, lambdas are only allowed from c11, please don't use ancient compiler
    auto atomItems = [&](uint32_t startPos) { // lambda, inner function
        char temp[5] = {0};
        m_audiofile.seek(startPos);
        m_audiofile.readBytes(temp, 4);
        atom.size = bigEndian((uint8_t*)temp, 4);
        if(!atom.size) atom.size = 4; // has no data, length is 0
        m_audiofile.readBytes(atom.name, 4);
        atom.name[4] = '\0';
        atom.pos = startPos;
        return atom;
    };
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    uint32_t stsdPos = 0;
    uint16_t stsdSize = 0;
    boolean  found = false;
    uint32_t seekpos = 0;
    uint32_t filesize = getFileSize();
    char     name[6][5] = {"moov", "trak", "mdia", "minf", "stbl", "stsz"};
    char     noe[4] = {0};

    if(!m_audiofile) return; // guard

    at.pos = 0;
    at.size = filesize;
    seekpos = 0;

    for(int i = 0; i < 6; i++) {
        found = false;
        while(seekpos < at.pos + at.size) {
            tmp = atomItems(seekpos);
            seekpos += tmp.size;
        //  log_i("tmp.name %s, tmp.size %i, seekpos %i", tmp.name, tmp.size, seekpos);
            if(strcmp(tmp.name, name[i]) == 0) {
                memcpy((void*)&at, (void*)&tmp, sizeof(tmp));
                found = true;
            }
            // log_i("name %s pos %d, size %d", tmp.name, tmp.pos, tmp.size);
            if(strcmp(tmp.name, "stsd") == 0) { // in stsd we can found mp4a atom that contains the audioitems
                stsdPos = tmp.pos;
                stsdSize = tmp.size;
            }
        }
        if(!found) goto noSuccess;
        seekpos = at.pos + 8; // 4 bytes size + 4 bytes name
    }
    seekpos += 8; // 1 byte version + 3 bytes flags + 4  bytes sample size
    m_audiofile.seek(seekpos);
    m_audiofile.readBytes(noe, 4); // number of entries
    m_stsz_numEntries = bigEndian((uint8_t*)noe, 4);
    // log_i("number of entries in stsz: %d", m_stsz_numEntries);
    m_stsz_position = seekpos + 4;
    if(stsdSize) {
        m_audiofile.seek(stsdPos);
        uint8_t data[128];
        m_audiofile.readBytes((char*)data, 128);
        int offset = specialIndexOf(data, "mp4a", stsdSize);
        if(offset > 0) {
            int channel = bigEndian(data + offset + 20, 2); // audio parameter must be set before starting
            int bps = bigEndian(data + offset + 22, 2);     // the aac decoder. There are RAW blocks only in m4a
            int srate = bigEndian(data + offset + 26, 4);   //
            setBitsPerSample(bps);
            setChannels(channel);
            setSampleRate(srate);
            setBitrate(bps * channel * srate);
            AUDIO_INFO("ch; %i, bps: %i, sr: %i", channel, bps, srate);
        }
    }
    m_audiofile.seek(0);
    return;

noSuccess:
    m_stsz_numEntries = 0;
    m_stsz_position = 0;
    log_e("m4a atom stsz not found");
    m_audiofile.seek(0);
    return;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::m4a_correctResumeFilePos() {
    // In order to jump within an m4a file, the exact beginning of an aac block must be found. Since m4a cannot be
    // streamed, i.e. there is no syncword, an imprecise jump can lead to a crash.

    if(!m_stsz_position) return m_audioDataStart; // guard

    typedef union {
        uint8_t  u8[4];
        uint32_t u32;
    } tu;
    tu uu;

    uint32_t i = 0, pos = m_audioDataStart;
    uint32_t filePtr = m_audiofile.position();
    bool found = false;
    m_audiofile.seek(m_stsz_position);

    while(i < m_stsz_numEntries) {
        i++;
        uu.u8[3] = m_audiofile.read();
        uu.u8[2] = m_audiofile.read();
        uu.u8[1] = m_audiofile.read();
        uu.u8[0] = m_audiofile.read();
        pos += uu.u32;
        if(pos >= m_resumeFilePos) {found = true; break;}
    }
    if(!found)  return -1; // not found

    m_audiofile.seek(filePtr); // restore file pointer
    return pos - m_resumeFilePos; // return the number of bytes to jump
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Audio::ogg_correctResumeFilePos() {
    // The starting point is the next OggS magic word
    vTaskDelay(1);
    // // log_w("in_resumeFilePos %i", resumeFilePos);

    auto find_sync_word = [&](uint8_t* pos, uint32_t av) -> int {
        int steps = 0;
        while(av--) {
            if(pos[steps] == 'O'){
                if(pos[steps + 1] == 'g') {
                    if(pos[steps + 2] == 'g') {
                        if(pos[steps + 3] == 'S') { // Check for the second part of magic word
                            return steps;   // Magic word found, return the number of steps
                        }
                    }
                }
            }
            steps++;
        }
        return -1; // Return -1 if OggS magic word is not found
    };

    uint8_t*       readPtr = InBuff.getReadPtr();
    size_t         av = InBuff.getMaxAvailableBytes();
    int32_t        steps = 0;

    if(av < InBuff.getMaxBlockSize()) return -1; // guard

    steps = find_sync_word(readPtr, av);
    if(steps == -1) return -1;
    return steps; // Return the number of steps to the sync word
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int32_t Audio::flac_correctResumeFilePos() {

    auto find_sync_word = [&](uint8_t* pos, uint32_t av) -> int {
        int steps = 0;
        while(av--) {
            char c = pos[steps];
            steps++;
            if(c == 0xFF) { // First byte of sync word found
                char nextByte = pos[steps];
                steps++;
                if(nextByte == 0xF8) {     // Check for the second part of sync word
                    return steps - 2;       // Sync word found, return the number of steps
                }
            }
        }
        return -1; // Return -1 if sync word is not found
    };

    uint8_t*       readPtr = InBuff.getReadPtr();
    size_t         av = InBuff.getMaxAvailableBytes();
    int32_t        steps = 0;

    if(av < InBuff.getMaxBlockSize()) return -1; // guard

    steps = find_sync_word(readPtr, av);
    if(steps == -1) return -1;
    return steps; // Return the number of steps to the sync word
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int32_t Audio::mp3_correctResumeFilePos() {

    // The SncronWord sequence 0xFF 0xF? can be part of valid audio data. Therefore, it cannot be ensured that the next 0xFFF is really the beginning
    // of a new MP3 frame. Therefore, the following byte is parsed. If the bitrate and sample rate match the one currently being played,
    // the beginning of a new MP3 frame is likely.

    const int16_t bitrateTab[3][3][15] PROGMEM = { {
        /* MPEG-1 */
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 }, /* Layer 1 */
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 }, /* Layer 2 */
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 }, /* Layer 3 */
        }, {
        /* MPEG-2 */
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 }, /* Layer 1 */
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 }, /* Layer 2 */
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 }, /* Layer 3 */
        }, {
        /* MPEG-2.5 */
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 }, /* Layer 1 */
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 }, /* Layer 2 */
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 }, /* Layer 3 */
    }, };

    auto find_sync_word = [&](uint8_t* pos, uint32_t av) -> int {
        int steps = 0;
        while(av--) {
            char c = pos[steps];
            steps++;
            if(c == 0xFF) { // First byte of sync word found
                char nextByte = pos[steps];
                steps++;
                if((nextByte & 0xF0) == 0xF0) { // Check for the second part of sync word
                    return steps - 2;           // Sync word found, return the number of steps
                }
            }
        }
        return -1; // Return -1 if sync word is not found
    };

    uint8_t        syncH, syncL, frame0;
    int32_t        steps = 0;
    const uint8_t* pos = InBuff.getReadPtr();
    uint8_t*       readPtr = InBuff.getReadPtr();
    size_t         av = InBuff.getMaxAvailableBytes();


    if(av < InBuff.getMaxBlockSize()) return -1; // guard

    while(true) {
        steps = find_sync_word(readPtr, av);
        if(steps == -1) return -1;
        readPtr += steps;
        syncH  = *(readPtr    ); (void)syncH; // readPtr[0];
        syncL  = *(readPtr + 1);
        frame0 = *(readPtr + 2);
        if((frame0 & 0b11110000) == 0b11110000){readPtr++; /* log_w("wrong bitrate index");                 */ continue;}
        if((frame0 & 0b00001100) == 0b00001100){readPtr++; /* log_w("wrong sampling rate frequency index"); */ continue;}
        int32_t  verIdx = (syncL >> 3) & 0x03;
        uint8_t  mpegVers = (verIdx == 0 ? MPEG25 : ((verIdx & 0x01) ? MPEG1 : MPEG2));
        uint8_t  brIdx = (frame0 >> 4) & 0x0f;
        uint8_t  srIdx = (frame0 >> 2) & 0x03;
        uint8_t  layer = 4 - ((syncL >> 1) & 0x03);
        if(srIdx == 3 || layer == 4 || brIdx == 15) {readPtr++; continue;}
        if(brIdx){
            uint32_t bitrate = ((int32_t) bitrateTab[mpegVers][layer - 1][brIdx]) * 1000;
            uint32_t samplerate = samplerateTab[mpegVers][srIdx];
            // log_w("syncH 0x%02X, syncL 0x%02X bitrate %i, samplerate %i", syncH, syncL, bitrate, samplerate);
            if(MP3GetBitrate() == bitrate && getSampleRate() == samplerate) break;
        }
        readPtr++;
    }
    // log_w("found sync word at %i  sync1 = 0x%02X, sync2 = 0x%02X", readPtr - pos, *readPtr, *(readPtr + 1));
    return readPtr - pos; // return the position of the first byte of the frame
}
#endif  // AUDIO_NO_SD_FS
//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint8_t Audio::determineOggCodec(uint8_t* data, uint16_t len) {
    // if we have contentType == application/ogg; codec cn be OPUS, FLAC or VORBIS
    // let's have a look, what it is
    int idx = specialIndexOf(data, "OggS", 6);
    if(idx != 0) {
        if(specialIndexOf(data, "fLaC", 6)) return CODEC_FLAC;
        return CODEC_NONE;
    }
    data += 27;
    idx = specialIndexOf(data, "OpusHead", 40);
    if(idx >= 0) { return CODEC_OPUS; }
    idx = specialIndexOf(data, "fLaC", 40);
    if(idx >= 0) { return CODEC_FLAC; }
    idx = specialIndexOf(data, "vorbis", 40);
    if(idx >= 0) { return CODEC_VORBIS; }
    return CODEC_NONE;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// separate task for decoding and outputting the data. 'playAudioData()' is started periodically and fetches the data from the InBuffer. This ensures
// that the I2S-DMA is always sufficiently filled, even if the Arduino 'loop' is stuck.
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void Audio::setAudioTaskCore(uint8_t coreID){  // Recommendation:If the ARDUINO RUNNING CORE is 1, the audio task should be core 0 or vice versa
    if(coreID > 1) return;
    stopAudioTask();
    xSemaphoreTake(mutex_audioTask, 0.3 * configTICK_RATE_HZ);
    m_audioTaskCoreId = coreID;
    xSemaphoreGive(mutex_audioTask);
    startAudioTask();
}

void Audio::startAudioTask() {
    if (m_f_audioTaskIsRunning) {
        log_i("Task is already running.");
        return;
    }
    m_f_audioTaskIsRunning = true;


    m_audioTaskHandle = xTaskCreateStaticPinnedToCore(
        &Audio::taskWrapper,    /* Function to implement the task */
        "PeriodicTask",         /* Name of the task */
        AUDIO_STACK_SIZE,       /* Stack size in words */
        this,                   /* Task input parameter */
        2,                      /* Priority of the task */
        xAudioStack,            /* Task stack */
        &xAudioTaskBuffer,      /* Memory for the task's control block */
        m_audioTaskCoreId       /* Core where the task should run */
    );
}

void Audio::stopAudioTask()  {
    if (!m_f_audioTaskIsRunning) {
        log_i("audio task is not running.");
        return;
    }
    xSemaphoreTake(mutex_audioTask, 0.3 * configTICK_RATE_HZ);
    m_f_audioTaskIsRunning = false;
    if (m_audioTaskHandle != nullptr) {
        vTaskDelete(m_audioTaskHandle);
        m_audioTaskHandle = nullptr;
    }
    xSemaphoreGive(mutex_audioTask);
}

void Audio::taskWrapper(void *param) {
    Audio *runner = static_cast<Audio*>(param);
    runner->audioTask();
}

void Audio::audioTask() {
    while (m_f_audioTaskIsRunning) {
        vTaskDelay(1 / portTICK_PERIOD_MS);  // periodically every x ms
        performAudioTask();
    }
    vTaskDelete(nullptr);  // Delete this task
}

void Audio::performAudioTask() {
    if(!m_f_running) return;
    if(!m_f_stream) return;
    if(m_codec == CODEC_NONE) return; // wait for codec is  set
    if(m_codec == CODEC_OGG)  return; // wait for FLAC, VORBIS or OPUS
    xSemaphoreTake(mutex_audioTask, 0.3 * configTICK_RATE_HZ);
    while(m_validSamples) {vTaskDelay(20 / portTICK_PERIOD_MS); playChunk();} // I2S buffer full
    playAudioData();
    xSemaphoreGive(mutex_audioTask);
}
uint32_t Audio::getHighWatermark(){
    UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(m_audioTaskHandle);
    return highWaterMark; // dwords
}
