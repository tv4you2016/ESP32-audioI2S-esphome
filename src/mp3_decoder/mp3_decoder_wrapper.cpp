#include "mp3_decoder_wrapper.h"

bool MP3Decoder::init() { return MP3Decoder_AllocateBuffers(); }

void MP3Decoder::clear() { MP3Decoder_ClearBuffer(); }

void MP3Decoder::reset() { MP3Decoder_FreeBuffers(); }

bool MP3Decoder::isValid() { return MP3Decoder_IsInit(); }

int32_t MP3Decoder::findSyncWord(uint8_t* buf, int32_t nBytes) { return MP3FindSyncWord(buf, nBytes); }

uint8_t MP3Decoder::getChannels() { return static_cast<uint8_t>(MP3GetChannels()); }

uint32_t MP3Decoder::getSampleRate() { return static_cast<uint32_t>(MP3GetSampRate()); }

uint8_t MP3Decoder::getBitsPerSample() { return static_cast<uint8_t>(MP3GetBitsPerSample()); }

uint32_t MP3Decoder::getBitRate() { return static_cast<uint32_t>(MP3GetBitrate()); }

uint32_t MP3Decoder::getAudioDataStart() { return 0; }

uint32_t MP3Decoder::getAudioFileDuration() { return 0; }

uint32_t MP3Decoder::getOutputSamples() { return static_cast<uint32_t>(MP3GetOutputSamps()); }

int32_t MP3Decoder::decode(uint8_t* inbuf, int32_t* bytesLeft, int16_t* outbuf) { return MP3Decode(inbuf, bytesLeft, outbuf); }

void MP3Decoder::setRawBlockParams(uint8_t /*param1*/, uint32_t /*param2*/, uint8_t /*param3*/, uint32_t /*param4*/, uint32_t /*param5*/) {
    // mp3 internal decoder determines channels/sample rate/bitdepth from frame headers
}

const char* MP3Decoder::getStreamTitle() { return ""; }

const char* MP3Decoder::whoIsIt() { return "MP3"; }

std::vector<uint32_t> MP3Decoder::getMetadataBlockPicture() { return {}; }

const char* MP3Decoder::arg1() { return ""; }

const char* MP3Decoder::arg2() { return ""; }

int32_t MP3Decoder::val1() { return 0; }

int32_t MP3Decoder::val2() { return 0; }
