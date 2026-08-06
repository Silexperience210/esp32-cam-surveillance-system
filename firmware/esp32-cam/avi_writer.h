/*
 * AVIWriter : enregistreur video AVI (MJPEG) minimaliste pour ESP32-CAM.
 * Les frames JPEG de la camera sont ecrites telles quelles sur la carte SD.
 * Compatible VLC, lecteurs Windows, Telegram (en piece jointe).
 * 
 * v3 : patch des fps reels mesures pour eviter la video acceleree
 */
#ifndef AVI_WRITER_H
#define AVI_WRITER_H

#include <FS.h>

class AVIWriter {
public:
  bool begin(File file, uint16_t width, uint16_t height, uint8_t fps, uint16_t maxFrames) {
    _f = file;
    _fps = fps;
    _frames = 0;
    _maxFrames = maxFrames;
    _width = width;
    _height = height;
    _offsets = (uint32_t*)malloc(maxFrames * sizeof(uint32_t));
    _sizes   = (uint32_t*)malloc(maxFrames * sizeof(uint32_t));
    if (!_offsets || !_sizes) return false;

    // ---- RIFF header ----
    _f.write((const uint8_t*)"RIFF", 4);
    w32(0);                       // taille RIFF (patchee a la fin)
    _f.write((const uint8_t*)"AVI ", 4);

    // ---- LIST hdrl ----
    _f.write((const uint8_t*)"LIST", 4);
    w32(192);
    _f.write((const uint8_t*)"hdrl", 4);

    // avih (56 octets)
    _f.write((const uint8_t*)"avih", 4);
    w32(56);
    _avihPos = _f.position();     // <- position pour patcher les fps reels
    w32(1000000UL / fps);         // microsec par frame (patche plus tard)
    w32(0);                       // max bytes/sec
    w32(0);                       // padding
    w32(0x10);                    // flags : HASINDEX
    _avihFramesPos = _f.position();
    w32(0);                       // total frames (patche a la fin)
    w32(0);                       // initial frames
    w32(1);                       // 1 stream
    w32(0);                       // suggested buffer
    w32(width);
    w32(height);
    w32(0); w32(0); w32(0); w32(0);

    // ---- LIST strl ----
    _f.write((const uint8_t*)"LIST", 4);
    w32(116);
    _f.write((const uint8_t*)"strl", 4);

    // strh (56 octets)
    _f.write((const uint8_t*)"strh", 4);
    w32(56);
    _f.write((const uint8_t*)"vids", 4);
    _f.write((const uint8_t*)"MJPG", 4);
    w32(0);                       // flags
    w32(0);                       // priority + language
    w32(0);                       // initial frames
    w32(1);                       // scale
    _strhRatePos = _f.position(); // <- position pour patcher le rate reel
    w32(fps);                     // rate (patche plus tard)
    w32(0);                       // start
    _strhLengthPos = _f.position();
    w32(0);                       // length en frames (patche a la fin)
    w32(0);                       // suggested buffer
    w32(0xFFFFFFFF);              // quality
    w32(0);                       // sample size
    w16(0); w16(0);               // rcFrame left, top
    w16(width); w16(height);      // rcFrame right, bottom

    // strf = BITMAPINFOHEADER (40 octets)
    _f.write((const uint8_t*)"strf", 4);
    w32(40);
    w32(40);                      // biSize
    w32(width);
    w32(height);
    w16(1);                       // biPlanes
    w16(24);                      // biBitCount
    _f.write((const uint8_t*)"MJPG", 4);
    w32((uint32_t)width * height * 2);
    w32(0); w32(0); w32(0); w32(0);

    // ---- LIST movi ----
    _f.write((const uint8_t*)"LIST", 4);
    _moviSizePos = _f.position();
    w32(0);                       // taille (patchee a la fin)
    _f.write((const uint8_t*)"movi", 4);
    _dataStart = _f.position();
    return true;
  }

  bool addFrame(const uint8_t* data, size_t len) {
    if (_frames >= _maxFrames) return false;
    _offsets[_frames] = (_f.position() - _dataStart) + 4;
    _sizes[_frames] = len;
    _f.write((const uint8_t*)"00dc", 4);
    w32(len);
    _f.write(data, len);
    if (len & 1) _f.write((uint8_t)0);  // alignement pair
    _frames++;
    return true;
  }

  uint32_t end(uint32_t realDurationMs = 0) {
    uint32_t idxPos = _f.position();

    // ---- idx1 ----
    _f.write((const uint8_t*)"idx1", 4);
    w32(_frames * 16);
    for (uint16_t i = 0; i < _frames; i++) {
      _f.write((const uint8_t*)"00dc", 4);
      w32(0x10);
      w32(_offsets[i]);
      w32(_sizes[i]);
    }
    uint32_t endPos = _f.position();

    // ---- patchs ----
    _f.seek(_moviSizePos);
    w32(4 + (idxPos - _dataStart));
    _f.seek(4);
    w32(endPos - 8);
    _f.seek(_avihFramesPos);
    w32(_frames);
    _f.seek(_strhLengthPos);
    w32(_frames);

    // Patch fps reels si duree mesuree disponible
    if (realDurationMs > 0 && _frames > 1) {
      uint32_t usPerFrame = realDurationMs * 1000UL / (_frames - 1);
      if (usPerFrame < 1000) usPerFrame = 1000;  // max 1000 fps
      uint32_t realFps = 1000000UL / usPerFrame;
      if (realFps < 1) realFps = 1;
      _f.seek(_avihPos);
      w32(usPerFrame);
      _f.seek(_strhRatePos);
      w32(realFps);
    }

    _f.seek(endPos);
    _f.flush();

    free(_offsets);
    free(_sizes);
    return _frames;
  }

  uint16_t frameCount() { return _frames; }

private:
  void w32(uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; _f.write(b, 4); }
  void w16(uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; _f.write(b, 2); }

  File _f;
  uint8_t _fps = 10;
  uint16_t _frames = 0, _maxFrames = 0, _width = 0, _height = 0;
  uint32_t _moviSizePos = 0, _dataStart = 0;
  uint32_t _avihPos = 0, _avihFramesPos = 0;
  uint32_t _strhRatePos = 0, _strhLengthPos = 0;
  uint32_t* _offsets = nullptr;
  uint32_t* _sizes = nullptr;
};

#endif
