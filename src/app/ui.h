// LunaShare user interface (Dear ImGui). Desktop: sidebar + tabs. Mobile: vertical screens with big buttons.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "lunashare_video.h"
#include "file_browser.h"
#include "imgui.h"
#include "music.h"

// where each movie was left (ui_movies.cpp): key = its path (or "lib:<id>" for a contact's file)
bool UpdateIsNewer(const std::string& tag, const std::string& mine);  // ("v1.6.0" vs "1.5.5")
void UpdateShutdown();
void DrawPin(ImDrawList* dl, ImVec2 c, float size, ImU32 col);  // a pushpin mark
bool LockOn(const ls::Settings& s);

namespace movpos {
struct Mark {
    double pos = 0, dur = 0;
    bool watched = false;
    int64_t at = 0;
};
void Init(const std::string& configDir);
bool Get(const std::string& key, Mark& m);
void Set(const std::string& key, double pos, double dur);  // (near the end: watched, from the start next time)
void Forget(const std::string& key);
void Save(bool force = false);
nlohmann::json Export();               // (settings backup)
void Import(const nlohmann::json& j);  // (newer marks win)
}  // namespace movpos

// file names in a natural order ("Episode 2" before "Episode 10") - ui_watch.cpp
bool NaturalLess(const std::string& a, const std::string& b);

class App {
public:
    explicit App(ls::Engine& e);

    // Builds the whole UI for this frame. Call between ImGui::NewFrame() and ImGui::Render().
    void Frame();
    void OnDrop(const std::vector<std::string>& paths);
    // files opened with LunaShare (the file manager's "Open with", a second start, Android's open / share): shown in
    // the matching place - pictures in the viewer, movies in the movie player, songs in the music player, archives and
    // comics in theirs; send = to a chat. Taken up at the next frame.
    void OpenPaths(const std::vector<std::string>& paths, bool send);
    void HandleOpenPaths();
    std::vector<std::string> openPathsPending;
    bool openPathsSend = false;
    // Returns true when the app may quit now; otherwise shows a confirmation first.
    bool RequestQuit();
    // Two-finger pinch from SDL_MULTIGESTURE (dDist as reported, x/y in window pixels).
    void OnPinch(float dDist, float x, float y);
    bool ViewerOpen() const { return viewer.open; }

    bool quitConfirmed = false;
    bool wantAttention = false;  // flash the taskbar / vibrate
    std::string windowTitle = "LunaShare";
    bool busy = false;           // transfers running -> keep redrawing
    float keyboardInset = 0;     // px hidden by the on-screen keyboard (mobile)
    // the typed text shown again just above the on-screen keyboard (the field itself may be under it); tapping it
    // moves the cursor. main.cpp keeps the taps inside mirrorRect away from ImGui (they'd end the typing)
    void DrawTextMirror();
    // the sleep timer (ui_sleep.cpp): mode -1 off, 0 at sleepAt, 1 after this song, 2 after this movie
    void SleepSet(int minutes, int mode);
    void SleepStopAll();
    void SleepTick();
    std::string SleepLabel();
    void SleepPopup();
    void DrawSleepAsk();
    int sleepMode = -1;
    int64_t sleepAt = 0;
    bool sleepAsked = false, sleepAskOpen = false, sleepOpen = false;
    // chat history: archives, clearing, restoring, asking for it back (ui_archive.cpp)
    void OpenChatHistory(const std::string& peer);
    void DrawChatHistoryPopup();
    // a chat's notifications: off for a while / for good, or back on
    bool MuteItem(const std::string& key, float h, bool button = false);  // (a menu line / button: opens the choices, or turns them back on)
    void DrawMutePopup();
    void SyncTick();  // my other devices: playlists and settings
    void ClipTick();  // ... and the clipboard (an option)
    void ClipReceived(const nlohmann::json& d);
    // unsent text, per chat (kept in drafts.json)
    std::string DraftGet(const std::string& peer);
    void DraftSet(const std::string& peer, const std::string& text);
    void DraftsSave();
    // a newer version on GitHub (once a day, an option; or "Check now")
    void UpdateTick();
    // app lock (PIN / fingerprint)
    void LockTick();
    void LockNow();
    void UnlockWith(const std::string& pin);
    void DrawLockScreen();
    void DrawLockSettings(float h);
    void DrawLockSetPopup();
    void DrawLockEncPopup();  // the PIN once more: the chat history encrypted with it
    bool locked = false, lockInit = false, lockWasBg = false, lockSecure = false, lockBioAsked = false, lockForgot = false, lockSetOpen = false;
    int64_t lockBgSince = 0, lockWaitUntil = 0;
    int lockWrong = 0;
    float lockShake = 0;
    std::string lockEntry, lockPhraseBuf, lockNew1, lockNew2, lockSetErr, lockEncPin, lockEncErr;
    bool lockEncOpen = false;
    void UpdateCheckNow();
    void DrawUpdatePopup();
    std::string updateTag, updateUrl, updateNotes;
    bool updateOpen = false, updateAsked = false;
    nlohmann::json AppDataExport();           // settings file: playlists, music folders, sound, movie positions
    int AppDataImport(const nlohmann::json& j);  // (returns how many playlists there are now)
    std::string muteKey;
    bool muteOpen = false;
    void DrawHistoryAskBanner(const std::string& peer);
    // search in a chat / all chats (ui_archive.cpp)
    void DrawChatSearch(const std::string& peer);
    void OpenChatSearch() { searchOn = true; searchFocus = true; }
    bool searchOn = false, searchFocus = false, searchAll = false;
    std::string replyMid, editMid, replyFor;  // answering / changing a message (in replyFor's chat)
    std::string searchText, searchJump, searchFlash, searchFor;
    int searchAt = -1;
    int64_t searchFlashAt = 0;
    std::string histPeer;
    bool histOpen = false, histClear = false, histKeep = true, histAll = false;
    std::string histConfirmDelete;
    float mirrorRect[4] = {0, 0, 0, 0};  // x0 y0 x1 y1 (empty: none)
    float mirrorTapX = -1, mirrorTapY = -1;
    int mirrorFirstLine = 0;

    // style rebuild requests
    int styleGeneration = 0;

private:
    enum class Tab { Chat, Transfers, History, Library, Gallery, Movies, Music, Archives, Torrents, Devices, Nearby, Settings };
    enum class MScreen { Home, Chat, Transfers, History, Nearby, Settings, Devices, Library, Music, Movies, Gallery, Archives, Torrents };

    struct Toast {
        ls::EventType type;
        std::string title, text, peer;
        uint64_t tid = 0;
        float ttl = 6.0f;
    };

    void PullEvents();
    std::vector<std::string> SortedPeers(bool people = true);
    bool IsMobileLayout();

    // layouts
    void DrawDesktop();
    void DrawMobile();
    void DrawSidebar();
    void DrawMobileHome();
    void DrawMobileNav();

    // screens (shared)
    void DrawChat(const std::string& peer);
    void DrawMessages(const std::string& peer);
    // emoji reactions: pills under a bubble, hover button, quick row + full picker popups
    void DrawReactions(const std::string& peer, ls::ChatMessage& m, ImVec2 b0, ImVec2 b1);
    void ReactHoverButton(const ls::ChatMessage& m, ImVec2 b0, ImVec2 b1);
    bool QuickReactionRow(const std::string& peer, const std::string& mid, const std::string& current);
    void DrawReactionPopups(const std::string& peer);
    std::string reactMid;
    bool openReactPick = false, openReactMore = false;
    void DrawBatchCard(const std::string& peer, const ls::ChatMessage& m, float width);
    // polls: the card in the chat (tap an answer), and making one
    void DrawPollCard(const std::string& peer, const ls::ChatMessage& m, float width);
    void OpenPollNew(const std::string& peer);
    void DrawPollNewPopup();
    std::string pollPeer, pollQ;
    std::vector<std::string> pollOpts;
    bool pollMulti = false, pollOpen = false;
    void DrawTransferRow(const std::shared_ptr<ls::Transfer>& t, bool compact);
    void DrawTransferActions(const std::shared_ptr<ls::Transfer>& t);
    void DrawTransfers();
    void DrawHistory();
    void DrawDevices();
    void DrawDeviceDetails(const std::string& peer);
    // verifying a contact by QR code: mine shown, theirs scanned with the camera
    void DrawVerifyQrPopup();
    bool verifyQrOpen = false, verifyQrScan = false;
    std::string verifyQrPeer, verifyQrMsg;
    int verifyQrResult = 0;  // after a scan: 1 the same keys, -1 different, 2 not a LunaShare code
    void DrawNearby();
    void DrawSettings();
    void DrawLibrary();  // ui_library.cpp
    // groups (ui_groups.cpp)
    void OpenNewGroup();
    void OpenAddToGroup(const std::string& key);
    void OpenGroupMembers(const std::string& key);
    void GroupButtons(const std::string& key, float size);
    void DrawGroupPopups();
    // watch together (ui_watch.cpp)
    void WatchSync();
    void DrawWatch();
    void DrawWatchBanner(const std::string& peer);
    void OpenWatchPicker(const std::string& peer);
    void OpenMoviePlayer();                 // the movie player: a movie alone (people can be added later)
    // ---- music (ui_music.cpp)
    void DrawMusic();
public:
    void MusicTick();
private:
    void DrawMusicNow(bool big);
    void DrawMusicMini();
    void MusicSoundPopup();
    // the Movies tab (ui_movies.cpp)
    void DrawMovies();
    void MovieRescan();
    void OpenMovies();                // the Movies tab / screen
    bool movieScanned = false, movieRescanWanted = false;
    std::string movieSearch;
    int movieSort = 0;
    // the Gallery (ui_gallery.cpp): pictures and videos of the camera and the folders, by album or by month
    void DrawGallery();
    void GalleryRescan();
    void OpenGallery();
    void GalleryBackupPopup();
    bool galleryScanned = false, galleryRescanWanted = false;
    std::string gallerySearch, galleryAlbum;  // (album: a folder opened; "" = the albums / everything)
    int galleryView = 0;                      // 0 by month, 1 albums
    std::set<std::string> gallerySel;         // picked (long press / the Select button)
    bool gallerySelecting = false, galleryDeleteAsk = false;
    // torrents (ui_torrents.cpp): here with the torrent module, or on one of my computers
public:
    void TorrentOpenSource(const std::string& source);  // a magnet link or a .torrent file: asks where to / on which computer
private:
    void DrawTorrents();
    void OpenTorrents();
    void TorrentPopups();
    void WatchTorrent(const std::string& device, const std::string& id, int file, int64_t size, const std::string& name);
    void WatchExtTick();
    void WatchExtEnd();
    std::function<int(int64_t, uint8_t*, int)> watchExtReader;
    std::string watchExtId, watchExtTorrent, watchExtDevice;
    std::shared_ptr<std::atomic<bool>> watchExtStarved;
    bool torrentAddOpen = false, torrentSettingsOpen = false, torrentFilesOpen = false, torrentRemoveOpen = false, torrentRemoveFiles = false;
    std::string torrentAddSource, torrentAddDir, torrentAddErr, torrentAddDevice, torrentMagnetBuf;
    int torrentAddThen = 0;  // (on a computer: 0 send it here when done, 1 keep it there)
    std::string torrentFilesId, torrentRemoveId, torrentRemoveDevice, torrentRemoveName, torrentDevice;
    // the Archives tab (ui_archives.cpp) and comics (ui_comics.cpp)
public:
    void OpenArchive(const std::string& path, const std::string& password = "");
    void StartArchiveCreate(const std::vector<std::string>& inputs);
    void OpenComic(const std::string& path, const std::string& password = "");
    std::string archiveDoneMsg, archiveDoneDir;
private:
    void DrawArchives();
    void DrawArchiveOpen();
    void DrawArchiveJob();
    void ArchiveTick();
    void DrawComicShelf(const std::vector<std::string>& paths, bool busy);
    void ComicTick();
    void ComicFrame();
    bool ComicOpening() const;
    void ArchivesRescan();
    void OpenArchives();
    bool ArchiveBack();
    void ArchiveOpenEntry(const std::string& entry);
    void ArchiveExtractPopup();
    void ArchiveCreatePopup();
    std::vector<std::string> ArchiveFolders();
    bool archivesScanned = false, archivesRescanWanted = false, archivesComics = false;
    std::string archivesSearch;
    bool archiveExtractOpen = false, archiveExtractSub = true;
    std::vector<std::string> archiveExtractOnly;
    std::string archiveExtractTo;
    bool archiveCreateOpen = false;
    std::vector<std::string> archiveCreateInputs;
    std::string archiveCreateName, archiveCreateTo;
    int archiveCreateFormat = 0;
    // listening together / contacts' songs (ui_listen.cpp)
    void MusicExternalTick();
    void MusicPlayLib(const std::vector<ls::LibFile>& list, int at, const std::string& who);
    void MusicLibOpen();
    void MusicLibEnd();
    int MusicLibNext(bool skip);
    void MusicListenAdd(const std::vector<std::string>& paths, double startAt = -1, bool quiet = false);
    void MusicListenPlaylistPopup();
    int musicListenStart = 0;          // starting to listen together with: 0 what's on, 1 a playlist, 2 nothing yet
    std::string musicListenPl;         // (that playlist)
    bool musicListenPlOpen = false;
    void MusicListenPopup();
    void DrawListenStrip();
    void DrawListenQueue();
    void DrawListenBanner(const std::string& peer);
    bool musicListenOpen = false, listenFollowing = false;
    std::vector<ls::LibFile> musicLibList;
    int musicLibAt = -1;
    std::string musicLibId, musicLibWho;
    uint64_t musicLibSrc = 0;
    bool musicSoundOpen = false, musicSoundDirty = false;
    void MusicRefresh();
    const music::Track* MusicTrack(const std::string& path);
    void MusicTrackList(const std::vector<const music::Track*>& list, const std::string& playlistId, bool showAlbum);
    void MusicTrackMenu(const std::string& path, const std::string& playlistId, int playlistIndex);
    void MusicPlaylistMove(const std::string& id, int from, int to);  // to = -1: remove
    void DrawLyrics(ImDrawList* dl, ImVec2 a, ImVec2 b, double pos);
    std::string musicLyricsFor;
    std::vector<std::pair<double, std::string>> musicLyrics;
    bool musicLyricsSynced = false, musicLyricsOn = true;
    float musicLyricsY = 0, musicLyricsScroll = 0;
    std::vector<std::string> musicListShown;  // (the playlist's songs as shown)
    void MusicGroupButtons(const std::vector<const music::Track*>& list, const std::string& sendFolder);
    void MusicSend(const std::vector<std::string>& paths, const std::string& folder);
    void MusicSendPopup();
    void MusicPlaylistPopup();
    std::vector<music::Track> musicTracks;
    std::map<std::string, int> musicByPath;
    uint64_t musicVer = 0;
    int musicView = 0, musicSort = 0;
    std::string musicSearch, musicGroup, musicSendName, musicNewName;
    std::set<std::string> musicSel;
    std::vector<std::pair<std::string, std::string>> musicSendFiles;
    std::vector<std::string> musicAddPaths;
    bool musicSendOpen = false, musicAddOpen = false, musicNowShown = false;
    float musicDrag = -1;
    void DrawWatchResume();                 // "the movie is on" strip outside its chat (movie player, library)
    void WatchAddPopup();                   // host: add a person / group to the movie
    void ToggleLoop();                      // chats: videos and audio start again at the end
    bool watchShown = false, watchFailed = false;
    bool watchFromViewer = false;
    // previous / next movie: the videos in the movie's folder (or the Shared-folder list it was started from)
    std::string watchDirOf;
    std::vector<std::string> watchDirList;
    std::vector<ls::LibFile> watchLibList;
    int watchLibAt = -1;
    int WatchNeighbor(int dir);       // index in the list of the previous (-1) / next (+1) movie, -1 = none
    void WatchStep(int dir);  // the movie was opened from the picture viewer: leaving it reopens the viewer
    std::string watchOpened;
    int64_t watchLastSeek = 0;
    float watchDrag = -1;
    int64_t watchActive = 0;  // last time someone touched / moved the mouse (controls hide a few seconds later)
    int64_t watchShownSince = 0;
    bool watchCtlShown = false, watchTapHides = false;
    bool watchWasPlaying = false;     // (hide the controls when it starts playing)
    uint64_t watchHideSid = 0;
    int watchOrient = -1;             // Android, by the movie's shape: 0 sideways, 1 upright (-1 not set)
    int64_t watchNextAt = 0;
    double watchSubDelay = 0;
    double watchAbA = -1, watchAbB = -1;  // repeating a part (A-B); -1 = not set
    uint64_t watchAbSid = 0;              // (the movie it was set for)
    int watchPipState = -1;           // Android: what picture-in-picture was last told (-1 nothing)
    bool watchInPip = false;         // subtitles later (+) / sooner (-), this movie
    // phone gestures: double tap a side = 10 s back / on, a swipe up / down = brightness (left) / volume (right)
    int watchGesture = 0;             // 0 none, 1 brightness, 2 volume
    float watchGestureStart = 0, watchGestureVal = 0, watchBrightness = -1;
    int64_t watchFlashAt = 0;
    std::string watchFlash;
    double watchResumedFrom = 0;      // (continued from where it was left: shown for a moment, with "from the start")
    int64_t watchResumedAt = 0, watchPosSaved = 0;
    std::string watchPosKey;
    bool watchFullscreen = false, watchMovie = false, watchAwake = false, watchDevSub = false;
    std::set<std::string> watchSubsAdded;
    bool watchSubFollow = true;       // viewer: subtitles as the host has them (until picking others)
    std::string watchSubApplied;      // ... the host's choice that was applied
    int64_t watchSubCheck = 0;
    void WatchTracksPopup();
    void GroupCallStrip(float tile, bool onVideo);  // the people in a group call (tap: watch their video)
    std::string CallPeerName(const ls::CallPeer& p);
    bool openGroupDlg = false, groupDlgAdd = false, openGroupMembers = false;
    std::string groupDlgKey, groupNameBuf;
    std::set<std::string> groupPick;
    std::string librarySearch;
    void DrawProfilePicture(float h);

    // popups / overlays
    void DrawPopups();
    // calls (only when the optional media module is installed)
    void DrawCallBar();
    void DrawIncomingCall();
    bool CallButton(const std::string& peer, float size, bool video = false);
    bool WebcamButton(const std::string& peer, float size);  // PC: "use this phone as webcam"; phone: "send my camera to this PC"
    bool WebcamButtonShown(const std::string& peer) const;
    void DrawCallButtons(float bw, float h, bool withVideo);
    int CallButtonCount(bool withVideo) const;
    void DrawVideoStage(ImVec2 size, bool phone);
    void DrawVideoOverlay();  // phone: full-screen video call
    void DrawCameraPanel();
    bool VideoQualityControls(ls::Settings& s, float w, float h);  // resolution + frame rate; true when changed
    bool VideoVisible() const;
    bool videoFull = false;       // phone: full-screen video shown
    bool videoAutoOpened = false; // opened once per call
    bool videoBig = false;        // desktop: video takes the whole chat area
    bool camPanel = false;
    std::vector<LsvControl> camCtl;
    double camCtlTime = -1;
    void DrawCallSettings(float fieldW, float h, bool& changed);
    void DrawPrivateContacts(float fieldW, float h, bool& changed);
    void DrawInvitesPopup();
    bool openInvites = false;
    Tab navLast[8] = {Tab::Chat, Tab::Transfers, Tab::Gallery, Tab::Archives, Tab::Devices, Tab::Settings, Tab::Chat, Tab::Chat};  // the page each section was left on
    // the welcome of a first start and "Add someone" (ui_welcome.cpp)
    void WelcomeInit();
    void WelcomeDone();
    void DrawWelcome();
    void OpenAddPerson(int section);  // 0 nearby, 1 QR code, 2 Internet (invite codes), 3 get the app; -1 = where it was
    void DrawAddPersonPopup();
    bool DrawAddPersonBody(float width);
    void HandleAddCode(const std::string& text);
    void AddPersonTick();
    int UseInviteCode(const std::string& code, std::string& msg);
    std::string CreateInvite(const std::string& forWhom);
    bool welcomeInit = false;
    int welcomeStep = -1;  // 0..2 while the welcome shows
    std::string welcomeName;
    bool addPersonOpen = false, addPersonClose = false, addGoConnect = false, addGoInvites = false, addScan = false, addMsgErr = false;
    int addSection = 0;
    std::string addMsg, addCodeBuf, addInviteFor, addInviteCode;
    std::string connectPendingPub, connectPendingVerify;  // someone whose QR code was scanned: connecting to them
    int64_t connectPendingUntil = 0;
    void AudioCallControls(float fieldW, float h, bool& changed);
    // picture viewer (ui_viewer.cpp)
    void OpenViewer(const std::string& peer, const std::string& path, bool all);
    // a set list (a gallery folder, a comic's pages, the pictures next to an opened file); captions optional
    void OpenViewerList(const std::vector<std::string>& paths, int index, const std::vector<std::string>& captions = {});
    void RebuildViewerList();
    void DrawViewer();
    void ViewerZoomAt(ImVec2 pt, float factor);
    void ViewerGo(int delta);
    void DrawThumbnails(const std::string& peer, const std::vector<std::shared_ptr<ls::Transfer>>& items, float width);
    void DrawAudioItems(const std::vector<std::shared_ptr<ls::Transfer>>& items, float width);
    void DrawToasts();
    void OpenSendTo(const std::vector<std::string>& paths);
    // "a link for anyone" (ui_links.cpp)
    void OpenShareLink(const std::vector<std::string>& paths);
    void DrawShareLinkPopups();
    void DrawShareLinksList();
    struct ShareLinkUi;
    std::shared_ptr<ShareLinkUi> shareUi;
    void CopySelectedMessages(const std::string& peer);
    void SelectPeer(const std::string& peer);

    bool DevicePeerRow(const std::string& peer, bool selected, float height);

    ls::Engine& eng;
    Tab tab = Tab::Chat;
    bool tabForce = false;
    MScreen screen = MScreen::Home;
    std::vector<MScreen> backStack;
    std::string selectedPeer;
    std::string chatInput;
    std::vector<std::string> pendingForward;  // messages' text to pass on (with pendingSend: their files)
    bool pendingShareText = false;            // pendingForward is text shared with LunaShare (not forwarded)
    // sending files from the app: photos made smaller first when that's on (on a thread), then SendPaths
    void SendFilesUI(const std::string& peer, const std::vector<std::string>& paths);
    void ForwardMessages(const std::string& peer, const std::vector<std::string>& mids);
    void DrawPinnedBar(const std::string& peer);  // the chat's pinned messages (a tap goes there, the next tap to the next)
    void DrawStarredPopup();                      // all starred messages
    int pinIdx = 0;
    bool starredOpen = false;
    bool focusInput = false;
    size_t lastMsgCount = 0;
    std::string lastMsgPeer;
    bool scrollToBottom = false;

    std::set<std::string> selMsgs;  // selected message ids
    int lastSelIndex = -1;
    bool selectionMode = false;     // touch: taps toggle selection
    std::string textViewBuf;
    bool openTextView = false;
    std::string contextMid;
    std::string recPath, recPeer;  // voice / video message being recorded
    void DrawVideoMessage(const std::string& peer, float inputH);
    int vmsgCams = 0;
    bool emojiOpen = false;  // emoji panel under the messages
    std::vector<std::string> deleteMids;  // waiting for confirmation
    bool deleteEveryoneOk = false;        // all of them are ours, so "delete for everyone" is possible
    bool openDeleteConfirm = false;
    void AskDelete(const std::string& peer, const std::vector<std::string>& mids);
    void DrawDeleteConfirm(const std::string& peer);

    std::vector<std::string> pendingSend;
    bool openSendTo = false;
    std::string sendToFilter;

    std::set<uint64_t> expandedBatches;

    FileBrowser browser;
    enum class BrowseFor { None, SendFiles, DownloadDir, Export, Import, Avatar, BackupSave, BackupOpen, Watch, WatchSubs, Movie, MusicFolder, MovieFolder, SyncFolder, GalleryFolder, ArchiveOpen, ArchiveInputs, ArchiveDest, ArchiveCreateDest, ArchiveFolder, TorrentFile, TorrentDir, TorrentSaveDir } browseFor = BrowseFor::None;
    // account (ui_account.cpp): Settings section, linking a device, recovery phrase, backup files
    void DrawAccountSettings(float fieldW, float h);
    void DrawAccountPopups();
    void DrawFolderSyncSettings(float h);  // folders that go to my other devices by themselves
    bool openLinkPick = false, openLinkAsk = false, openPhrase = false, openRestore = false, openBackupPass = false;
    std::string linkTarget;               // device we offered to link (waiting for its answer)
    std::string linkAskPeer, linkAskCode, linkAskName;  // a device asks this one to join its account
    std::string phraseShown, restoreBuf, backupPass, backupPass2, backupPath, accountError;
    bool backupSaving = false;            // password popup: saving (true) or opening (false) a backup
    std::string removeDeviceAsk;
    std::string usernameBuf, findUserBuf, findUserAcc;  // usernames: claim field, search field, person to open once connected
    static std::string UsernameError(const std::string& e);
    bool openRemoveDevice = false;
    // profile picture: round picture (or coloured circle with the initial) of a device; peer "" = this device
    void DrawAvatar(ImDrawList* dl, ImVec2 center, float diameter, const std::string& peer, const std::string& name);
    void DrawAvatarEditor();
    std::string avatarSrc;  // picture being cropped
    float avatarZoom = 1, avatarCx = 0.5f, avatarCy = 0.5f;
    bool openAvatarEditor = false;
    bool exportHistory = true, exportIdentity = false, importIdentity = false;

    std::string historyFilter;
    std::set<uint64_t> historySel;
    std::vector<std::string> reshareFiles;

    std::string devicesSel;
    std::string renameBuf;
    std::string renamePeer;
    bool openRename = false;

    std::string manualHost;
    int manualPort = 47820;
    std::string qrSsid, qrPass;
    std::string joinSsid, joinPass;
    struct WifiOffer { std::string peer, ssid, pass; };
    std::vector<WifiOffer> wifiOffers;
    bool openWifiOffer = false;

    std::vector<Toast> toasts;
    std::string infoMessage;
    bool openInfo = false;
    bool openQuit = false;

    struct Viewer {
        bool open = false, requestOpen = false;
        bool all = false;          // false: pictures of this chat, true: all shared pictures
        bool fixed = false;        // a set list (OpenViewerList): not rebuilt from the chats
        std::string peer;
        std::vector<std::string> paths, captions;
        int index = 0;
        float zoom = 1.0f;
        ImVec2 pan{0, 0};
        float swipeX = 0;
        ImVec2 areaMin{0, 0}, areaMax{0, 0};
        float uiAlpha = 1.0f;
        std::string videoPath;     // video being played in the viewer
        bool slideshow = false;    // on its own to the next one every few seconds
        double slideT = 0;
        bool canDelete = false;    // (the gallery: pictures of mine)
        std::string comic;         // reading this comic (its archive): the page is remembered
        bool rtl = false;          // pages go right to left (manga)
        bool closeRequest = false; // (closed from outside: something else was opened)
    } viewer;

    bool openIncomingCall = false;
    int settingsShownFrame = -100;
    ls::Settings edit;
    bool editDirty = false;
};
