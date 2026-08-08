#pragma once
#include <gtkmm.h>
#include <atomic>
#include <string>
#include <vector>

#include "dm/device.hpp"
#include "dm/smart.hpp"
#include "dm/partition.hpp"
#include "dm/filesystem.hpp"
#include "dm/mdadm.hpp"
#include "dm/locate.hpp"
#include "dm/firmware.hpp"
#include "dm/recovery.hpp"

namespace dm {

// Desktop (gtkmm) equivalent of TuiApp. Offers the same set of actions as the
// ncurses TUI (src/tui/tui_app.cpp) but structured the way a graphical app
// naturally wants to be: a single top-level window with a notebook of tabs,
// dialogs for drill-down/detail views, and background threads (marshaled back
// to the GTK main loop via Glib::Dispatcher) for anything slow, instead of the
// TUI's blocking modal-menu loop.
class GuiApp : public Gtk::Window {
public:
    GuiApp();

private:
    // ---- column records for the TreeViews -------------------------------
    struct DiskColumns : public Gtk::TreeModelColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> path;
        Gtk::TreeModelColumn<Glib::ustring> size;
        Gtk::TreeModelColumn<Glib::ustring> status;
        Gtk::TreeModelColumn<Glib::ustring> statusColor;
        Gtk::TreeModelColumn<Glib::ustring> model;
        Gtk::TreeModelColumn<Glib::ustring> system;
        DiskColumns() { add(path); add(size); add(status); add(statusColor); add(model); add(system); }
    };

    struct ArrayColumns : public Gtk::TreeModelColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> device;
        Gtk::TreeModelColumn<Glib::ustring> level;
        Gtk::TreeModelColumn<Glib::ustring> size;
        Gtk::TreeModelColumn<Glib::ustring> state;
        Gtk::TreeModelColumn<Glib::ustring> stateColor;
        // -1 for a real, currently-assembled array (indexes into mdadm_.listArrays()
        // results by device path instead); >=0 indexes discoveredInactive_ for a row
        // found on-disk but not currently assembled.
        Gtk::TreeModelColumn<int> discoveredIndex;
        ArrayColumns() { add(device); add(level); add(size); add(state); add(stateColor); add(discoveredIndex); }
    };

    struct FirmwareColumns : public Gtk::TreeModelColumnRecord {
        Gtk::TreeModelColumn<int> index; // into firmwareInfos_
        Gtk::TreeModelColumn<Glib::ustring> devicePath;
        Gtk::TreeModelColumn<Glib::ustring> vendorName;
        Gtk::TreeModelColumn<Glib::ustring> current;
        Gtk::TreeModelColumn<Glib::ustring> latest;
        Gtk::TreeModelColumn<Glib::ustring> updateAvailable;
        FirmwareColumns() { add(index); add(devicePath); add(vendorName); add(current); add(latest); add(updateAvailable); }
    };

    struct RaidMemberColumns : public Gtk::TreeModelColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> device;
        Gtk::TreeModelColumn<Glib::ustring> state;
        RaidMemberColumns() { add(device); add(state); }
    };

    // ---- setup helpers ----------------------------------------------------
    void buildDisksTab();
    void buildArraysTab();
    void buildFirmwareTab();
    void buildRecoveryTab();

    void refreshDisks();
    void refreshArrays();
    void refreshFirmware(bool refreshFromNetwork);

    // ---- Disks tab handlers -------------------------------------------
    std::string selectedDiskPath();
    void onDisksRowActivated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column);
    void onDiskDetailsClicked();
    void onDiskPartitionsClicked();
    void onDiskLocateClicked();

    void showSmartDetailDialog(const std::string& diskPath);
    void showPartitionsDialog(const std::string& diskPath);
    void showMountManageDialog(const BlockDevice& dev);

    // ---- Arrays tab handlers --------------------------------------------
    std::string selectedArrayPath();
    // If the current Arrays-tab selection is a discovered-but-not-assembled row,
    // fills *outDisc (when non-null) and returns true.
    bool selectedIsDiscovered(DiscoveredArray* outDisc);
    // Shows an explanatory dialog and returns true if the current selection is a
    // discovered-but-not-assembled row -- callers that only make sense for an
    // active array (replace/grow/stop/mount/...) call this first and bail if true.
    bool guardNotDiscovered(const std::string& actionName);
    void onArraysRowActivated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column);
    void onArrayDetailClicked();
    void onAssembleDiscoveredClicked();
    void onCreateArrayClicked();
    void onReplaceDriveClicked();
    void onAddSpareClicked();
    void onFailRemoveClicked();
    void onGrowArrayClicked();
    void onStopArrayClicked();
    void onArrayMountClicked();

    void showArrayDetailDialog(const std::string& mdPath);
    void showCreateArrayDialog();
    void showReplaceDriveDialog(const std::string& mdPath);
    // Runs `work` on a background thread while showing a modal progress dialog
    // driven by a Glib::Dispatcher; blocks the calling (UI) thread inside the
    // dialog's nested main loop, same shape as ui::showProgress in the TUI.
    void runWithProgressDialog(const std::string& title,
        const std::function<void(std::atomic<bool>& cancel,
                                  const std::function<void(double, const std::string&)>& update)>& work);
    void watchRebuild(const std::string& mdPath, const std::string& title);

    // ---- Firmware tab handlers -------------------------------------------
    void onFirmwareRefreshClicked();
    void onFirmwareUpdateClicked();
    void onFirmwareSelectionChanged();

    // ---- Recovery tab handlers -------------------------------------------
    void onDdrescueBrowseClicked();
    void onDdrescueStartClicked();
    void onLaunchTestdiskClicked();
    void onLaunchPhotorecClicked();

    // ---- generic dialog helpers -------------------------------------------
    bool confirmYesNo(const std::string& title, const std::string& message,
                       Gtk::MessageType type = Gtk::MESSAGE_WARNING);
    void infoDialog(const std::string& title, const std::string& message,
                     Gtk::MessageType type = Gtk::MESSAGE_INFO);
    // Text-entry prompt. Returns false if the user cancelled.
    bool promptInput(const std::string& title, const std::string& label,
                      const std::string& initial, std::string& out);

    // ---- managers (shared dm_core, same as TuiApp) -------------------------
    DeviceManager devices_;
    SmartManager smart_;
    PartitionManager partitions_;
    FilesystemManager filesystems_;
    MdadmManager mdadm_;
    LocateManager locate_;
    FirmwareManager firmware_;
    RecoveryManager recovery_;

    // ---- top level layout ---------------------------------------------
    Gtk::Box vbox_{Gtk::ORIENTATION_VERTICAL};
    Gtk::InfoBar rootInfoBar_;
    Gtk::Label rootInfoLabel_;
    Gtk::Notebook notebook_;

    // ---- Disks tab ---------------------------------------------------
    Gtk::Box disksTab_{Gtk::ORIENTATION_VERTICAL, 4};
    Gtk::ScrolledWindow disksScroll_;
    Gtk::TreeView disksView_;
    Glib::RefPtr<Gtk::ListStore> disksStore_;
    DiskColumns diskCols_;
    Gtk::Box disksButtonBox_{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Button disksRefreshBtn_{"Refresh"};
    Gtk::Button disksDetailsBtn_{"SMART Details"};
    Gtk::Button disksPartitionsBtn_{"Partitions"};
    Gtk::Button disksLocateBtn_{"Locate"};

    // ---- Arrays tab ----------------------------------------------------
    Gtk::Box arraysTab_{Gtk::ORIENTATION_VERTICAL, 4};
    Gtk::ScrolledWindow arraysScroll_;
    Gtk::TreeView arraysView_;
    Glib::RefPtr<Gtk::ListStore> arraysStore_;
    ArrayColumns arrayCols_;
    std::vector<DiscoveredArray> discoveredInactive_; // backing store for discoveredIndex rows
    Gtk::Box arraysButtonBox_{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Button arraysRefreshBtn_{"Refresh"};
    Gtk::Button arraysDetailBtn_{"Detail / Rebuild status"};
    Gtk::Button arraysAssembleBtn_{"Assemble"};
    Gtk::Button arraysCreateBtn_{"Create Array"};
    Gtk::Button arraysReplaceBtn_{"Replace Drive"};
    Gtk::Button arraysAddSpareBtn_{"Add Spare"};
    Gtk::Button arraysFailRemoveBtn_{"Fail+Remove Member"};
    Gtk::Button arraysGrowBtn_{"Grow"};
    Gtk::Button arraysStopBtn_{"Stop Array"};
    Gtk::Button arraysMountBtn_{"Mount/Manage"};

    // ---- Firmware tab ----------------------------------------------------
    Gtk::Box firmwareTab_{Gtk::ORIENTATION_VERTICAL, 4};
    Gtk::Label firmwareUnavailableLabel_;
    Gtk::ScrolledWindow firmwareScroll_;
    Gtk::TreeView firmwareView_;
    Glib::RefPtr<Gtk::ListStore> firmwareStore_;
    FirmwareColumns firmwareCols_;
    Gtk::Box firmwareButtonBox_{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Button firmwareRefreshBtn_{"Refresh metadata"};
    Gtk::Button firmwareUpdateBtn_{"Update"};
    std::vector<FirmwareInfo> firmwareInfos_;
    bool firmwareAvailable_ = false;

    // ---- Recovery tab ----------------------------------------------------
    Gtk::Box recoveryTab_{Gtk::ORIENTATION_VERTICAL, 8};
    Gtk::Frame ddrescueFrame_{"Image a failing disk/partition/array with ddrescue"};
    Gtk::Grid ddrescueGrid_;
    Gtk::Entry ddrescueSourceEntry_;
    Gtk::Entry ddrescueDestEntry_;
    Gtk::Button ddrescueBrowseBtn_{"Browse..."};
    Gtk::Entry ddrescueMapEntry_;
    Gtk::Button ddrescueStartBtn_{"Start imaging"};
    Gtk::Frame testdiskFrame_{"Partition table / file recovery"};
    Gtk::Box testdiskBox_{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::Button launchTestdiskBtn_{"Launch testdisk..."};
    Gtk::Button launchPhotorecBtn_{"Launch photorec..."};

    // Spawns argv in the user's terminal emulator (testdisk/photorec need a
    // real controlling tty, so they cannot be embedded in-process). Tries
    // x-terminal-emulator first, then xterm.
    void spawnInTerminal(const std::vector<std::string>& innerArgv);
};

} // namespace dm
