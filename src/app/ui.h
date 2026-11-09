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
}  // namespace movpos

// file names in a natural order ("Episode 2" before "Episode 10") - ui_watch.cpp
bool NaturalLess(const std::string& a, const std::string& b);

class App {
public:
    explicit App(ls::Engine& e);

    // Builds the whole UI for this frame. Call between ImGui::NewFrame() and ImGui::Render().
    void Frame();
    void OnDrop(const std::vector<std::string>& paths);
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
    // chat history: archives, clearing, restoring, asking for it back (ui_archive.cpp)
    void OpenChatHistory(const std::string& peer);
    void DrawChatHistoryPopup();
    void DrawHistoryAskBanner(const std::string& peer);
    std::string histPeer;
    bool histOpen = false, histClear = false, histKeep = true, histAll = false;
    std::string histConfirmDelete;
    float mirrorRect[4] = {0, 0, 0, 0};  // x0 y0 x1 y1 (empty: none)
    float mirrorTapX = -1, mirrorTapY = -1;
    int mirrorFirstLine = 0;

    // style rebuild requests
    int styleGeneration = 0;

private:
    enum class Tab { Chat, Transfers, History, Library, Movies, Music, Devices, Nearby, Settings };
    enum class MScreen { Home, Chat, Transfers, History, Nearby, Settings, Devices, Library, Music, Movies };

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
    void DrawTransferRow(const std::shared_ptr<ls::Transfer>& t, bool compact);
    void DrawTransferActions(const std::shared_ptr<ls::Transfer>& t);
    void DrawTransfers();
    void DrawHistory();
    void DrawDevices();
    void DrawDeviceDetails(const std::string& peer);
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
    void AudioCallControls(float fieldW, float h, bool& changed);
    // picture viewer (ui_viewer.cpp)
    void OpenViewer(const std::string& peer, const std::string& path, bool all);
    void RebuildViewerList();
    void DrawViewer();
    void ViewerZoomAt(ImVec2 pt, float factor);
    void ViewerGo(int delta);
    void DrawThumbnails(const std::string& peer, const std::vector<std::shared_ptr<ls::Transfer>>& items, float width);
    void DrawAudioItems(const std::vector<std::shared_ptr<ls::Transfer>>& items, float width);
    void DrawToasts();
    void OpenSendTo(const std::vector<std::string>& paths);
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
    enum class BrowseFor { None, SendFiles, DownloadDir, Export, Import, Avatar, BackupSave, BackupOpen, Watch, WatchSubs, Movie, MusicFolder, MovieFolder } browseFor = BrowseFor::None;
    // account (ui_account.cpp): Settings section, linking a device, recovery phrase, backup files
    void DrawAccountSettings(float fieldW, float h);
    void DrawAccountPopups();
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
        std::string peer;
        std::vector<std::string> paths, captions;
        int index = 0;
        float zoom = 1.0f;
        ImVec2 pan{0, 0};
        float swipeX = 0;
        ImVec2 areaMin{0, 0}, areaMax{0, 0};
        float uiAlpha = 1.0f;
        std::string videoPath;     // video being played in the viewer
    } viewer;

    bool openIncomingCall = false;
    int settingsShownFrame = -100;
    ls::Settings edit;
    bool editDirty = false;
};
