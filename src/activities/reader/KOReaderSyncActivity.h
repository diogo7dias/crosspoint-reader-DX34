#pragma once
#include <Epub.h>

#include <functional>
#include <memory>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/ActivityWithSubactivity.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 *
 * Runs with the book RELEASED: the caller pre-computes the local KOReader
 * position + chapter name and frees Epub/Section before entering, so the TLS
 * handshake has heap to work with (an open book starves the WiFi driver's
 * DMA buffers — upstream releases the epub here for the same reason). The
 * epub is reloaded on demand only AFTER the network fetch, for mapping the
 * remote position back to a local page.
 */
class KOReaderSyncActivity final : public ActivityWithSubactivity {
 public:
  using OnCancelCallback = std::function<void()>;
  // remoteAnchor: HTML id embedded in the remote XPath (may be empty). The
  // reader floors the percentage-estimated page to this anchor's page so a
  // position just past a chapter heading never lands in the previous chapter.
  using OnSyncCompleteCallback =
      std::function<void(int newSpineIndex, int newPageNumber, const std::string& remoteAnchor)>;

  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                KOReaderPosition localKoPos, std::string localChapterName, OnCancelCallback onCancel,
                                OnSyncCompleteCallback onSyncComplete)
      : ActivityWithSubactivity("KOReaderSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        remoteProgress{},
        remotePosition{},
        localProgress(std::move(localKoPos)),
        localChapterName(std::move(localChapterName)),
        onCancel(std::move(onCancel)),
        onSyncComplete(std::move(onSyncComplete)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  // Loaded on demand AFTER the network fetch (see class comment); null while
  // the TLS handshake runs.
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format, pre-computed by the caller while the
  // epub was still loaded (for display + upload).
  KOReaderPosition localProgress;
  // Local chapter title, pre-computed by the caller (empty = no TOC entry).
  std::string localChapterName;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  OnCancelCallback onCancel;
  OnSyncCompleteCallback onSyncComplete;

  void onWifiSelectionComplete(bool success);
  bool ensureEpubLoaded();
  void performSync();
  void performUpload();
};
