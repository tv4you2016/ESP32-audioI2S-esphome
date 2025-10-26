#pragma once

#include "../Audio.h"
#include "mp3_decoder.h"
#include <vector>

class MP3Decoder : public Decoder {
  public:
    MP3Decoder(Audio& audioRef) : Decoder(audioRef), audio(audioRef) {}
    ~MP3Decoder() { reset(); }

    bool init() override;
    void clear() override;
    void reset() override;
    bool isValid() override;
    int32_t findSyncWord(uint8_t* buf, int32_t nBytes) override;
    uint8_t getChannels() override;
    uint32_t getSampleRate() override;
    uint8_t getBitsPerSample() override;
    uint32_t getBitRate() override;
    uint32_t getAudioDataStart() override;
    uint32_t getAudioFileDuration() override;
    uint32_t getOutputSamples() override;
    int32_t decode(uint8_t* inbuf, int32_t* bytesLeft, int16_t* outbuf) override;
    void setRawBlockParams(uint8_t param1, uint32_t param2, uint8_t param3, uint32_t param4, uint32_t param5) override;
    const char* getStreamTitle() override;
    const char* whoIsIt() override;
    std::vector<uint32_t> getMetadataBlockPicture() override;
    const char* arg1() override;
    const char* arg2() override;
    int32_t val1() override;
    int32_t val2() override;

  private:
    Audio& audio;
};
