// ufc3 - host-side music player for user-supplied tracks.
//
// Decodes any file Windows Media Foundation can read (wav, mp3, m4a/aac, wma,
// flac) to float PCM and streams it through its own XAudio2 engine, next to
// the emulated audio. One track at a time; Play() replaces the current one,
// Stop() fades it out.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/logging.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace ufc3_music {

class MusicPlayer {
 public:
  static MusicPlayer& Get() {
    static MusicPlayer player;
    return player;
  }

  // Starts `path` from the beginning (stopping any current track at once).
  void Play(const std::filesystem::path& path, float volume, bool loop) {
    std::lock_guard lock(mutex_);
    Join();
    stop_.store(false);
    abort_.store(false);
    paused_.store(false);
    playing_.store(true);
    thread_ = std::thread([this, path, volume, loop] { Run(path, volume, loop); });
  }

  // Fades the current track out over `fade` seconds (0 = cut). Never blocks:
  // the stream thread finishes the fade on its own.
  void Stop(float fade = 0.75f) {
    fade_seconds_.store(fade);
    stop_.store(true);
  }

  // Holds / continues the current track (the game's pause_all / resume_all).
  void Pause(bool paused) { paused_.store(paused); }

  bool IsPlaying() const { return playing_.load(); }

 private:
  ~MusicPlayer() {
    std::lock_guard lock(mutex_);
    Join();
  }

  // Cuts the current track and waits for its thread.
  void Join() {
    if (!thread_.joinable()) return;
    abort_.store(true);
    thread_.join();
  }

  void Run(std::filesystem::path path, float volume, bool loop) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool com = SUCCEEDED(hr);
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
      Stream(path, volume, loop);
      MFShutdown();
    } else {
      REXLOG_WARN("[music] Media Foundation startup failed");
    }
    if (com) CoUninitialize();
    playing_.store(false);
  }

  void Stream(const std::filesystem::path& path, float volume, bool loop) {
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) {
      REXLOG_WARN("[music] cannot open {} (unsupported format?)", path.string());
      return;
    }
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    IMFMediaType* want = nullptr;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    HRESULT hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want);
    want->Release();
    IMFMediaType* got = nullptr;
    WAVEFORMATEX* wfx = nullptr;
    UINT32 wfx_size = 0;
    if (FAILED(hr) ||
        FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got)) ||
        FAILED(MFCreateWaveFormatExFromMFMediaType(got, &wfx, &wfx_size))) {
      REXLOG_WARN("[music] cannot decode {}", path.string());
      if (got) got->Release();
      reader->Release();
      return;
    }
    got->Release();

    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(xaudio->CreateMasteringVoice(&master)) ||
        FAILED(xaudio->CreateSourceVoice(&voice, wfx))) {
      REXLOG_WARN("[music] XAudio2 init failed");
      if (xaudio) xaudio->Release();
      CoTaskMemFree(wfx);
      reader->Release();
      return;
    }
    const uint32_t rate = wfx->nSamplesPerSec;
    const uint32_t block = wfx->nBlockAlign;
    REXLOG_INFO("[music] playing {} ({} Hz, {} ch, loop={})", path.filename().string(), rate,
                wfx->nChannels, loop);
    CoTaskMemFree(wfx);

    voice->SetVolume(volume);
    voice->Start();
    // Ring of decoded buffers: XAudio2 reads from them until OnBufferEnd, so
    // keep each alive until the voice has moved past it.
    constexpr int kRing = 4;
    std::vector<uint8_t> ring[kRing];
    int next = 0;
    bool eof = false;
    float fade_left = -1.0f, fade_total = 0.0f;
    int empty_loops = 0;
    bool voice_paused = false;
    while (!abort_.load()) {
      bool want_pause = paused_.load() && !stop_.load();
      if (want_pause != voice_paused) {
        voice_paused = want_pause;
        if (voice_paused) {
          voice->Stop();
        } else {
          voice->Start();
        }
      }
      if (voice_paused) {
        Sleep(10);
        continue;
      }
      if (stop_.load() && fade_left < 0.0f) {
        fade_total = fade_left = fade_seconds_.load();
        if (fade_left <= 0.0f) break;
      }
      if (fade_left >= 0.0f) {
        fade_left -= 0.01f;
        if (fade_left <= 0.0f) break;
        voice->SetVolume(volume * fade_left / fade_total);
      }
      XAUDIO2_VOICE_STATE state;
      voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
      if (eof && state.BuffersQueued == 0) break;
      if (eof || state.BuffersQueued >= kRing - 1) {
        Sleep(10);
        continue;
      }
      // Decode ~0.25 s per buffer.
      std::vector<uint8_t>& buf = ring[next];
      buf.clear();
      while (buf.size() < rate / 4 * block) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags,
                                      nullptr, &sample))) {
          flags |= MF_SOURCE_READERF_ENDOFSTREAM;
        }
        if (sample) {
          IMFMediaBuffer* mb = nullptr;
          if (SUCCEEDED(sample->ConvertToContiguousBuffer(&mb))) {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(mb->Lock(&p, nullptr, &len))) {
              buf.insert(buf.end(), p, p + len);
              mb->Unlock();
            }
            mb->Release();
          }
          sample->Release();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
          if (loop) {
            PROPVARIANT pos;
            PropVariantInit(&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = 0;
            reader->SetCurrentPosition(GUID_NULL, pos);
            if (buf.empty() && ++empty_loops > 2) eof = true;  // nothing decodable
            break;
          } else {
            eof = true;
            break;
          }
        }
      }
      if (!buf.empty()) {
        empty_loops = 0;
        XAUDIO2_BUFFER xb = {};
        xb.AudioBytes = static_cast<UINT32>(buf.size());
        xb.pAudioData = buf.data();
        if (eof) xb.Flags = XAUDIO2_END_OF_STREAM;
        voice->SubmitSourceBuffer(&xb);
        next = (next + 1) % kRing;
      }
    }
    voice->Stop();
    voice->FlushSourceBuffers();
    voice->DestroyVoice();
    master->DestroyVoice();
    xaudio->Release();
    reader->Release();
  }

  std::mutex mutex_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> abort_{false};
  std::atomic<bool> paused_{false};
  std::atomic<bool> playing_{false};
  std::atomic<float> fade_seconds_{0.0f};
};

}  // namespace ufc3_music
