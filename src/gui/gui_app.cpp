#include "gui_app.hpp"

#include <glib.h>

#include <atomic>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace dm {

namespace {

std::string human(uint64_t n) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(v < 10 ? 2 : 1) << v << units[u];
    return os.str();
}

std::string healthGlyph(const SmartReport& r, bool atRisk) {
    if (!r.supported) return "?";
    if (atRisk) return "!";
    return r.healthy ? "OK" : "FAIL";
}

std::string healthColor(const std::string& glyph) {
    if (glyph == "OK") return "#2e7d32";   // green
    if (glyph == "!") return "#b8860b";    // yellow/amber
    if (glyph == "FAIL") return "#c62828"; // red
    return "#757575";                      // grey ("?")
}

// Modal dialog that drives a Gtk::ProgressBar from a worker thread. All
// worker-thread -> UI communication goes through Glib::Dispatcher, which is
// the only thread-safe way to wake up the GTK main loop from another thread;
// the worker must never touch any Gtk::Widget directly.
class ProgressDialog : public Gtk::Dialog {
public:
    ProgressDialog(Gtk::Window& parent, const std::string& title)
        : Gtk::Dialog(title, parent, true) {
        set_default_size(480, 130);
        get_content_area()->set_spacing(8);
        get_content_area()->set_border_width(10);
        label_.set_line_wrap(true);
        label_.set_xalign(0.0);
        label_.set_text("starting...");
        get_content_area()->pack_start(label_, Gtk::PACK_SHRINK);
        bar_.set_show_text(false);
        get_content_area()->pack_start(bar_, Gtk::PACK_SHRINK);
        add_button("Cancel", Gtk::RESPONSE_CANCEL);
        show_all_children();
        dispatcher_.connect(sigc::mem_fun(*this, &ProgressDialog::onDispatch));
    }

    std::atomic<bool>& cancelFlag() { return cancel_; }

    // Safe to call from the worker thread.
    void update(double percent, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            pendingPercent_ = percent;
            pendingMsg_ = msg;
        }
        dispatcher_.emit();
    }

    // Safe to call from the worker thread; signals completion.
    void finish() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            done_ = true;
        }
        dispatcher_.emit();
    }

protected:
    void on_response(int response_id) override {
        if (response_id == Gtk::RESPONSE_CANCEL) cancel_.store(true);
        Gtk::Dialog::on_response(response_id);
    }

private:
    void onDispatch() {
        double p;
        std::string m;
        bool d;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            p = pendingPercent_;
            m = pendingMsg_;
            d = done_;
        }
        label_.set_text(m);
        bar_.set_fraction(std::min(1.0, std::max(0.0, p / 100.0)));
        if (d) response(Gtk::RESPONSE_OK);
    }

    Gtk::Label label_;
    Gtk::ProgressBar bar_;
    Glib::Dispatcher dispatcher_;
    std::mutex mutex_;
    double pendingPercent_ = 0.0;
    std::string pendingMsg_;
    bool done_ = false;
    std::atomic<bool> cancel_{false};
};

} // namespace

// ============================================================ construction

GuiApp::GuiApp() {
    set_title("disk-manager");
    set_default_size(950, 620);
    set_border_width(4);
    add(vbox_);

    rootInfoBar_.set_message_type(Gtk::MESSAGE_WARNING);
    rootInfoBar_.set_show_close_button(true);
    rootInfoLabel_.set_line_wrap(true);
    rootInfoLabel_.set_xalign(0.0);
    rootInfoLabel_.set_text(
        "Not running as root. Viewing disks and SMART data works read-only, but "
        "partitioning, formatting, mounting, RAID management, firmware updates, "
        "and recovery all require root. Re-run with sudo for full functionality.");
    if (auto* box = dynamic_cast<Gtk::Box*>(rootInfoBar_.get_content_area()))
        box->pack_start(rootInfoLabel_, Gtk::PACK_SHRINK);
    rootInfoBar_.signal_response().connect([this](int) { rootInfoBar_.hide(); });
    vbox_.pack_start(rootInfoBar_, Gtk::PACK_SHRINK);

    vbox_.pack_start(notebook_, Gtk::PACK_EXPAND_WIDGET);

    buildDisksTab();
    buildArraysTab();
    buildFirmwareTab();
    buildRecoveryTab();

    show_all_children();
    if (isRoot()) rootInfoBar_.hide();
}

// ================================================================ Disks tab

void GuiApp::buildDisksTab() {
    disksStore_ = Gtk::ListStore::create(diskCols_);
    disksView_.set_model(disksStore_);
    disksView_.append_column("Device", diskCols_.path);
    disksView_.append_column("Size", diskCols_.size);
    {
        auto* col = Gtk::manage(new Gtk::TreeViewColumn("Health"));
        auto* renderer = Gtk::manage(new Gtk::CellRendererText());
        col->pack_start(*renderer, false);
        col->add_attribute(renderer->property_text(), diskCols_.status);
        col->add_attribute(renderer->property_foreground(), diskCols_.statusColor);
        disksView_.append_column(*col);
    }
    disksView_.append_column("Model", diskCols_.model);
    disksView_.append_column("", diskCols_.system);
    disksView_.signal_row_activated().connect(sigc::mem_fun(*this, &GuiApp::onDisksRowActivated));

    disksScroll_.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    disksScroll_.add(disksView_);
    disksScroll_.set_vexpand(true);
    disksTab_.set_border_width(6);
    disksTab_.pack_start(disksScroll_, Gtk::PACK_EXPAND_WIDGET);

    disksButtonBox_.pack_start(disksRefreshBtn_, Gtk::PACK_SHRINK);
    disksButtonBox_.pack_start(disksDetailsBtn_, Gtk::PACK_SHRINK);
    disksButtonBox_.pack_start(disksPartitionsBtn_, Gtk::PACK_SHRINK);
    disksButtonBox_.pack_start(disksLocateBtn_, Gtk::PACK_SHRINK);
    disksTab_.pack_start(disksButtonBox_, Gtk::PACK_SHRINK);

    disksRefreshBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::refreshDisks));
    disksDetailsBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDiskDetailsClicked));
    disksPartitionsBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDiskPartitionsClicked));
    disksLocateBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDiskLocateClicked));

    notebook_.append_page(disksTab_, "Disks");
    refreshDisks();
}

void GuiApp::refreshDisks() {
    disksStore_->clear();
    auto disks = devices_.listDisks();
    std::string rootDisk = devices_.rootDiskName();
    for (auto& d : disks) {
        auto report = smart_.query(d.path);
        bool risk = smart_.isAtRisk(report);
        std::string glyph = healthGlyph(report, risk);
        auto row = *disksStore_->append();
        row[diskCols_.path] = d.path;
        row[diskCols_.size] = human(d.sizeBytes);
        row[diskCols_.status] = glyph;
        row[diskCols_.statusColor] = healthColor(glyph);
        row[diskCols_.model] = d.model.empty() ? "(unknown model)" : d.model;
        row[diskCols_.system] = (!rootDisk.empty() && d.kname == rootDisk) ? "system disk" : "";
    }
}

std::string GuiApp::selectedDiskPath() {
    auto iter = disksView_.get_selection()->get_selected();
    if (!iter) return "";
    Glib::ustring p = (*iter)[diskCols_.path];
    return p;
}

void GuiApp::onDisksRowActivated(const Gtk::TreeModel::Path&, Gtk::TreeViewColumn*) {
    onDiskDetailsClicked();
}

void GuiApp::onDiskDetailsClicked() {
    std::string path = selectedDiskPath();
    if (path.empty()) { infoDialog("Disks", "Select a disk first."); return; }
    showSmartDetailDialog(path);
}

void GuiApp::onDiskPartitionsClicked() {
    std::string path = selectedDiskPath();
    if (path.empty()) { infoDialog("Disks", "Select a disk first."); return; }
    showPartitionsDialog(path);
    refreshDisks();
}

void GuiApp::onDiskLocateClicked() {
    std::string path = selectedDiskPath();
    if (path.empty()) { infoDialog("Disks", "Select a disk first."); return; }
    if (locate_.isActive(path)) {
        locate_.stop(path);
        infoDialog("Locate", "Stopped locating " + path);
        return;
    }
    auto method = locate_.bestMethodFor(path);
    if (method == LocateMethod::None) {
        infoDialog("Locate", "No way to identify this drive was found\n(no enclosure LED support and drive not readable).");
        return;
    }
    locate_.start(path, 0);
    std::string how = method == LocateMethod::Enclosure
        ? "Lighting the enclosure locate LED for this slot."
        : "No addressable LED found -- driving repeated read-only accesses so the\n"
          "drive's activity light flickers. Watch for the blinking drive, then come\n"
          "back here and click Locate again to stop.";
    infoDialog("Locate " + path, how);
}

void GuiApp::showSmartDetailDialog(const std::string& diskPath) {
    auto r = smart_.query(diskPath);
    Gtk::Dialog dlg("SMART: " + diskPath, *this, true);
    dlg.set_default_size(580, 500);
    dlg.add_button("Close", Gtk::RESPONSE_CLOSE);
    auto* content = dlg.get_content_area();
    content->set_spacing(6);
    content->set_border_width(8);

    Gtk::Label summary;
    summary.set_xalign(0.0);
    summary.set_line_wrap(true);
    std::ostringstream os;
    if (!r.supported) {
        os << "SMART not available for " << diskPath << "\n";
        if (!r.error.empty()) os << "Error: " << r.error;
    } else {
        os << "Overall health: " << (r.healthy ? "PASSED" : "FAILED") << "\n";
        os << "At-risk heuristic: " << (smart_.isAtRisk(r) ? "YES -- consider replacing" : "no") << "\n";
        if (!r.modelFamily.empty()) os << "Model family: " << r.modelFamily << "\n";
        if (!r.firmwareVersion.empty()) os << "Firmware: " << r.firmwareVersion << "\n";
        if (r.temperatureC) os << "Temperature: " << *r.temperatureC << " C\n";
        if (r.powerOnHours) os << "Power-on hours: " << *r.powerOnHours << "\n";
        if (r.powerCycles) os << "Power cycles: " << *r.powerCycles << "\n";
        if (r.reallocatedSectors) os << "Reallocated sectors: " << *r.reallocatedSectors << "\n";
        if (r.pendingSectors) os << "Pending sectors: " << *r.pendingSectors << "\n";
        if (r.uncorrectableSectors) os << "Uncorrectable sectors: " << *r.uncorrectableSectors << "\n";
        if (r.nvmePercentageUsed) os << "NVMe percentage used: " << (int)*r.nvmePercentageUsed << "%\n";
        if (r.nvmeMediaErrors) os << "NVMe media errors: " << *r.nvmeMediaErrors << "\n";
    }
    summary.set_text(os.str());
    content->pack_start(summary, Gtk::PACK_SHRINK);

    if (r.supported && !r.attributes.empty()) {
        struct AttrColumns : public Gtk::TreeModelColumnRecord {
            Gtk::TreeModelColumn<int> id, value, worst, threshold;
            Gtk::TreeModelColumn<Glib::ustring> name, raw, failed;
            AttrColumns() { add(id); add(name); add(value); add(worst); add(threshold); add(raw); add(failed); }
        } cols;
        auto store = Gtk::ListStore::create(cols);
        for (auto& a : r.attributes) {
            auto row = *store->append();
            row[cols.id] = a.id;
            row[cols.name] = a.name;
            row[cols.value] = a.value;
            row[cols.worst] = a.worst;
            row[cols.threshold] = a.threshold;
            row[cols.raw] = a.rawValue;
            row[cols.failed] = a.failed ? "FAILED" : "";
        }
        auto* tv = Gtk::manage(new Gtk::TreeView(store));
        tv->append_column("ID", cols.id);
        tv->append_column("Name", cols.name);
        tv->append_column("Value", cols.value);
        tv->append_column("Worst", cols.worst);
        tv->append_column("Thresh", cols.threshold);
        tv->append_column("Raw", cols.raw);
        tv->append_column("Failed", cols.failed);
        auto* scroll = Gtk::manage(new Gtk::ScrolledWindow());
        scroll->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        scroll->add(*tv);
        scroll->set_vexpand(true);
        content->pack_start(*scroll, Gtk::PACK_EXPAND_WIDGET);
    }

    dlg.show_all_children();
    dlg.run();
}

void GuiApp::showPartitionsDialog(const std::string& diskPath) {
    Gtk::Dialog dlg("Partitions: " + diskPath, *this, true);
    dlg.set_default_size(680, 440);
    dlg.add_button("Close", Gtk::RESPONSE_CLOSE);
    auto* content = dlg.get_content_area();
    content->set_spacing(6);
    content->set_border_width(8);

    struct PartColumns : public Gtk::TreeModelColumnRecord {
        Gtk::TreeModelColumn<int> number;
        Gtk::TreeModelColumn<Glib::ustring> path, size, fsType, flags;
        PartColumns() { add(number); add(path); add(size); add(fsType); add(flags); }
    } cols;
    auto store = Gtk::ListStore::create(cols);
    Gtk::TreeView tv(store);
    tv.append_column("#", cols.number);
    tv.append_column("Path", cols.path);
    tv.append_column("Size", cols.size);
    tv.append_column("Filesystem", cols.fsType);
    tv.append_column("Flags", cols.flags);
    Gtk::ScrolledWindow scroll;
    scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    scroll.add(tv);
    scroll.set_vexpand(true);
    content->pack_start(scroll, Gtk::PACK_EXPAND_WIDGET);

    Gtk::Label diskInfoLabel;
    diskInfoLabel.set_xalign(0.0);
    content->pack_start(diskInfoLabel, Gtk::PACK_SHRINK);

    auto reload = [&]() {
        store->clear();
        auto table = partitions_.read(diskPath);
        for (auto& p : table.partitions) {
            auto row = *store->append();
            row[cols.number] = p.number;
            row[cols.path] = p.path;
            row[cols.size] = human(p.sizeBytes);
            row[cols.fsType] = p.fsType.empty() ? "(no fs)" : p.fsType;
            row[cols.flags] = p.flags;
        }
        diskInfoLabel.set_text("Table: " + (table.label.empty() ? std::string("(none)") : table.label) +
                                "   Size: " + human(table.sizeBytes));
    };
    reload();

    Gtk::Box btnBox(Gtk::ORIENTATION_HORIZONTAL, 6);
    Gtk::Button createBtn("Create Partition");
    Gtk::Button mountBtn("Mount/Manage");
    Gtk::Button formatBtn("Format");
    Gtk::Button resizeBtn("Resize");
    Gtk::Button bootBtn("Toggle Boot Flag");
    Gtk::Button deleteBtn("Delete");
    for (auto* b : {&createBtn, &mountBtn, &formatBtn, &resizeBtn, &bootBtn, &deleteBtn})
        btnBox.pack_start(*b, Gtk::PACK_SHRINK);
    content->pack_start(btnBox, Gtk::PACK_SHRINK);

    auto getSelectedPartition = [&]() -> std::optional<PartitionEntry> {
        auto iter = tv.get_selection()->get_selected();
        if (!iter) return std::nullopt;
        int num = (*iter)[cols.number];
        auto table = partitions_.read(diskPath);
        for (auto& p : table.partitions)
            if (p.number == num) return p;
        return std::nullopt;
    };

    createBtn.signal_clicked().connect([&, this]() {
        auto table = partitions_.read(diskPath);
        if (table.label.empty() || table.label == "loop") {
            if (!confirmYesNo("Create partition table",
                    "Disk has no partition table (or is unpartitioned). Create a GPT table now?\n"
                    "This erases any existing data on " + diskPath + ".")) return;
            auto res = partitions_.createTable(diskPath, "gpt");
            if (!res.ok()) { infoDialog("Create table failed", res.stdErr, Gtk::MESSAGE_ERROR); return; }
        }
        std::string fsType, startStr, endStr;
        if (!promptInput("New partition", "Filesystem type hint (ext4, xfs, fat32, linux-swap, or blank):", "ext4", fsType)) return;
        if (!promptInput("New partition", "Start offset in MiB:", "1", startStr) || startStr.empty()) return;
        if (!promptInput("New partition", "End offset in MiB (blank = rest of disk):", "", endStr)) return;
        uint64_t startBytes = 0, endBytes = 0;
        try { startBytes = std::stoull(startStr) * 1024ULL * 1024ULL; }
        catch (...) { infoDialog("Create partition", "Invalid start offset.", Gtk::MESSAGE_ERROR); return; }
        if (!endStr.empty()) {
            try { endBytes = std::stoull(endStr) * 1024ULL * 1024ULL; }
            catch (...) { infoDialog("Create partition", "Invalid end offset.", Gtk::MESSAGE_ERROR); return; }
        }
        auto res = partitions_.createPartition(diskPath, fsType, startBytes, endBytes);
        infoDialog("Create partition", res.ok() ? "Partition created." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        reload();
    });

    mountBtn.signal_clicked().connect([&, this]() {
        auto part = getSelectedPartition();
        if (!part) { infoDialog("Mount", "Select a partition first."); return; }
        auto devOpt = devices_.find(part->path);
        if (!devOpt) { infoDialog("Mount", "Could not read device info for " + part->path, Gtk::MESSAGE_ERROR); return; }
        showMountManageDialog(*devOpt);
        reload();
    });

    formatBtn.signal_clicked().connect([&, this]() {
        auto part = getSelectedPartition();
        if (!part) { infoDialog("Format", "Select a partition first."); return; }
        std::string fsType;
        if (!promptInput("Format " + part->path, "Filesystem type (ext4, xfs, btrfs, vfat, ntfs):", "ext4", fsType) || fsType.empty()) return;
        if (!confirmYesNo("Format " + part->path,
                "This ERASES ALL DATA on " + part->path + " (" + human(part->sizeBytes) + ").")) return;
        auto res = filesystems_.makeFilesystem(part->path, fsType);
        infoDialog("mkfs " + part->path, res.ok() ? "Filesystem created." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        reload();
    });

    resizeBtn.signal_clicked().connect([&, this]() {
        auto part = getSelectedPartition();
        if (!part) { infoDialog("Resize", "Select a partition first."); return; }
        std::string endStr;
        if (!promptInput("Resize " + part->path, "New end offset in MiB (from start of disk):", "", endStr) || endStr.empty()) return;
        uint64_t endBytes = 0;
        try { endBytes = std::stoull(endStr) * 1024ULL * 1024ULL; }
        catch (...) { infoDialog("Resize", "Invalid value.", Gtk::MESSAGE_ERROR); return; }
        if (!confirmYesNo("Resize " + part->path,
                "Resizing " + part->path + " does not touch the filesystem inside it.\n"
                "Grow the filesystem afterwards from the mount-management dialog.")) return;
        auto res = partitions_.resizePartition(diskPath, part->number, endBytes);
        infoDialog("Resize " + part->path, res.ok() ? "Partition table updated." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        reload();
    });

    bootBtn.signal_clicked().connect([&, this]() {
        auto part = getSelectedPartition();
        if (!part) { infoDialog("Boot flag", "Select a partition first."); return; }
        bool turnOn = part->flags.find("boot") == std::string::npos;
        auto res = partitions_.setFlag(diskPath, part->number, "boot", turnOn);
        infoDialog("Boot flag", res.ok() ? "Done." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        reload();
    });

    deleteBtn.signal_clicked().connect([&, this]() {
        auto part = getSelectedPartition();
        if (!part) { infoDialog("Delete", "Select a partition first."); return; }
        if (!confirmYesNo("Delete partition",
                "This DELETES partition " + part->path + " and everything on it.")) return;
        auto res = partitions_.deletePartition(diskPath, part->number);
        infoDialog("Delete", res.ok() ? "Deleted." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        reload();
    });

    dlg.show_all_children();
    dlg.run();
}

void GuiApp::showMountManageDialog(const BlockDevice& devIn) {
    std::string devicePath = devIn.path;
    Gtk::Dialog dlg("Mount management: " + devicePath, *this, true);
    dlg.set_default_size(500, 300);
    dlg.add_button("Close", Gtk::RESPONSE_CLOSE);
    auto* content = dlg.get_content_area();
    content->set_spacing(8);
    content->set_border_width(8);

    Gtk::Label status;
    status.set_xalign(0.0);
    status.set_line_wrap(true);
    content->pack_start(status, Gtk::PACK_SHRINK);

    auto currentDev = [&]() -> BlockDevice {
        auto d = devices_.find(devicePath);
        return d ? *d : devIn;
    };
    auto refreshStatus = [&]() {
        auto dev = currentDev();
        bool inFstab = filesystems_.isInFstab(dev.path);
        std::ostringstream os;
        os << "Device: " << dev.path << "\nFilesystem: " << (dev.fsType.empty() ? "(none)" : dev.fsType)
           << "\nCurrently mounted at: " << (dev.mountpoint.empty() ? "(not mounted)" : dev.mountpoint)
           << "\nIn /etc/fstab: " << (inFstab ? "yes" : "no");
        status.set_text(os.str());
    };
    refreshStatus();

    Gtk::Box btnBox(Gtk::ORIENTATION_VERTICAL, 6);
    Gtk::Button mountBtn("Mount now");
    Gtk::Button unmountBtn("Unmount");
    Gtk::Button growBtn("Grow filesystem to fill partition");
    Gtk::Button addFstabBtn("Add persistent entry to /etc/fstab");
    Gtk::Button removeFstabBtn("Remove entry from /etc/fstab");
    for (auto* b : {&mountBtn, &unmountBtn, &growBtn, &addFstabBtn, &removeFstabBtn})
        btnBox.pack_start(*b, Gtk::PACK_SHRINK);
    content->pack_start(btnBox, Gtk::PACK_SHRINK);

    mountBtn.signal_clicked().connect([&, this]() {
        auto dev = currentDev();
        std::string mp;
        if (!promptInput("Mount", "Mountpoint path:", "/mnt/" + dev.name, mp) || mp.empty()) return;
        auto res = filesystems_.mount(dev.path, mp, dev.fsType);
        infoDialog("Mount", res.ok() ? ("Mounted at " + mp) : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        refreshStatus();
    });
    unmountBtn.signal_clicked().connect([&, this]() {
        auto dev = currentDev();
        auto res = filesystems_.unmount(dev.mountpoint.empty() ? dev.path : dev.mountpoint);
        infoDialog("Unmount", res.ok() ? "Unmounted." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        refreshStatus();
    });
    growBtn.signal_clicked().connect([&, this]() {
        auto dev = currentDev();
        auto res = filesystems_.resize(dev.path, dev.fsType, dev.mountpoint);
        infoDialog("Resize filesystem", res.ok() ? "Filesystem grown." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        refreshStatus();
    });
    addFstabBtn.signal_clicked().connect([&, this]() {
        auto dev = currentDev();
        std::string mp, opts;
        if (!promptInput("fstab", "Mountpoint for fstab entry:",
                dev.mountpoint.empty() ? ("/mnt/" + dev.name) : dev.mountpoint, mp) || mp.empty()) return;
        if (!promptInput("fstab", "Mount options:", "defaults", opts)) return;
        auto res = filesystems_.addToFstab(dev.path, mp, dev.fsType, opts);
        infoDialog("fstab", res.ok() ? "Added (backup of /etc/fstab saved)." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        refreshStatus();
    });
    removeFstabBtn.signal_clicked().connect([&, this]() {
        auto dev = currentDev();
        auto res = filesystems_.removeFromFstab(dev.path);
        infoDialog("fstab", res.ok() ? "Removed (backup of /etc/fstab saved)." : ("Failed:\n" + res.stdErr),
                   res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
        refreshStatus();
    });

    dlg.show_all_children();
    dlg.run();
}

// =============================================================== Arrays tab

void GuiApp::buildArraysTab() {
    arraysStore_ = Gtk::ListStore::create(arrayCols_);
    arraysView_.set_model(arraysStore_);
    arraysView_.append_column("Device", arrayCols_.device);
    arraysView_.append_column("Level", arrayCols_.level);
    arraysView_.append_column("Size", arrayCols_.size);
    {
        auto* col = Gtk::manage(new Gtk::TreeViewColumn("State"));
        auto* renderer = Gtk::manage(new Gtk::CellRendererText());
        col->pack_start(*renderer, false);
        col->add_attribute(renderer->property_text(), arrayCols_.state);
        col->add_attribute(renderer->property_foreground(), arrayCols_.stateColor);
        arraysView_.append_column(*col);
    }
    arraysView_.signal_row_activated().connect(sigc::mem_fun(*this, &GuiApp::onArraysRowActivated));

    arraysScroll_.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    arraysScroll_.add(arraysView_);
    arraysScroll_.set_vexpand(true);
    arraysTab_.set_border_width(6);
    arraysTab_.pack_start(arraysScroll_, Gtk::PACK_EXPAND_WIDGET);

    for (auto* b : {&arraysRefreshBtn_, &arraysDetailBtn_, &arraysAssembleBtn_, &arraysCreateBtn_, &arraysReplaceBtn_,
                    &arraysAddSpareBtn_, &arraysFailRemoveBtn_, &arraysGrowBtn_, &arraysStopBtn_,
                    &arraysMountBtn_})
        arraysButtonBox_.pack_start(*b, Gtk::PACK_SHRINK);
    arraysTab_.pack_start(arraysButtonBox_, Gtk::PACK_SHRINK);

    arraysRefreshBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::refreshArrays));
    arraysDetailBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onArrayDetailClicked));
    arraysAssembleBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onAssembleDiscoveredClicked));
    arraysCreateBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onCreateArrayClicked));
    arraysReplaceBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onReplaceDriveClicked));
    arraysAddSpareBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onAddSpareClicked));
    arraysFailRemoveBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onFailRemoveClicked));
    arraysGrowBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onGrowArrayClicked));
    arraysStopBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onStopArrayClicked));
    arraysMountBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onArrayMountClicked));

    notebook_.append_page(arraysTab_, "RAID Arrays");
    refreshArrays();
}

void GuiApp::refreshArrays() {
    arraysStore_->clear();
    discoveredInactive_.clear();
    if (!mdadm_.available()) return;

    auto arrays = mdadm_.listArrays();
    for (auto& a : arrays) {
        auto row = *arraysStore_->append();
        row[arrayCols_.device] = a.device;
        row[arrayCols_.level] = a.level;
        row[arrayCols_.size] = human(a.arraySizeBytes);
        row[arrayCols_.state] = a.state + (a.degraded ? "  [DEGRADED]" : "");
        row[arrayCols_.stateColor] = a.degraded ? "#c62828" : "#2e7d32";
        row[arrayCols_.discoveredIndex] = -1;
    }

    // Scans every disk's RAID superblock directly, not just what's currently
    // assembled -- surfaces arrays after a reboot without mdadm.conf, disks moved
    // from another machine, or an array someone stopped earlier.
    for (auto& d : mdadm_.scanForArrays()) {
        if (d.active) continue; // already shown above via listArrays()
        discoveredInactive_.push_back(d);
        auto row = *arraysStore_->append();
        row[arrayCols_.device] = d.preferredDevice;
        row[arrayCols_.level] = d.level.empty() ? "raid?" : d.level;
        row[arrayCols_.size] = std::to_string(d.numDevices) + " member(s)";
        row[arrayCols_.state] = "NOT ASSEMBLED";
        row[arrayCols_.stateColor] = "#8a6d00";
        row[arrayCols_.discoveredIndex] = (int)discoveredInactive_.size() - 1;
    }
}

std::string GuiApp::selectedArrayPath() {
    auto iter = arraysView_.get_selection()->get_selected();
    if (!iter) return "";
    Glib::ustring d = (*iter)[arrayCols_.device];
    return d;
}

bool GuiApp::selectedIsDiscovered(DiscoveredArray* outDisc) {
    auto iter = arraysView_.get_selection()->get_selected();
    if (!iter) return false;
    int idx = (*iter)[arrayCols_.discoveredIndex];
    if (idx < 0) return false;
    if (outDisc && idx < (int)discoveredInactive_.size()) *outDisc = discoveredInactive_[idx];
    return true;
}

bool GuiApp::guardNotDiscovered(const std::string& actionName) {
    if (!selectedIsDiscovered(nullptr)) return false;
    infoDialog(actionName, "This array isn't assembled yet. Select it and click \"Assemble\" first.");
    return true;
}

void GuiApp::onArraysRowActivated(const Gtk::TreeModel::Path&, Gtk::TreeViewColumn*) {
    if (selectedIsDiscovered(nullptr)) onAssembleDiscoveredClicked();
    else onArrayDetailClicked();
}

void GuiApp::onArrayDetailClicked() {
    if (guardNotDiscovered("Detail")) return;
    std::string path = selectedArrayPath();
    if (path.empty()) { infoDialog("RAID", "Select an array first."); return; }
    showArrayDetailDialog(path);
}

void GuiApp::onAssembleDiscoveredClicked() {
    DiscoveredArray disc;
    if (!selectedIsDiscovered(&disc)) { infoDialog("Assemble", "Select a \"NOT ASSEMBLED\" array first."); return; }

    std::ostringstream os;
    os << "Found on-disk but not currently assembled:\n\n"
       << "Suggested name: " << disc.preferredDevice << "\n"
       << "Level: " << (disc.level.empty() ? "(unknown)" : disc.level) << "\n"
       << "Metadata version: " << disc.metadataVersion << "\n"
       << "UUID: " << disc.uuid << "\n"
       << "Members (" << disc.devices.size() << "):\n";
    for (auto& dev : disc.devices) os << "  " << dev << "\n";
    os << "\nAssemble this array now?";
    if (!confirmYesNo("Assemble array", os.str(), Gtk::MESSAGE_QUESTION)) return;

    auto res = mdadm_.assembleArray(disc.preferredDevice, disc.devices);
    infoDialog("Assemble", res.ok() ? "Assembled as " + disc.preferredDevice + "." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
    refreshArrays();
}

void GuiApp::onCreateArrayClicked() {
    showCreateArrayDialog();
    refreshArrays();
}

void GuiApp::onReplaceDriveClicked() {
    if (guardNotDiscovered("Replace drive")) return;
    std::string path = selectedArrayPath();
    if (path.empty()) { infoDialog("RAID", "Select an array first."); return; }
    showReplaceDriveDialog(path);
    refreshArrays();
}

void GuiApp::onAddSpareClicked() {
    if (guardNotDiscovered("Add spare")) return;
    std::string mdPath = selectedArrayPath();
    if (mdPath.empty()) { infoDialog("Add spare", "Select an array first."); return; }
    std::string dev;
    if (!promptInput("Add spare to " + mdPath, "Device path to add as spare (e.g. /dev/sdd1):", "", dev) || dev.empty()) return;
    auto res = mdadm_.addDevice(mdPath, dev);
    infoDialog("Add device", res.ok() ? "Added." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
    refreshArrays();
}

void GuiApp::onFailRemoveClicked() {
    if (guardNotDiscovered("Fail+remove")) return;
    std::string mdPath = selectedArrayPath();
    if (mdPath.empty()) { infoDialog("Fail+remove", "Select an array first."); return; }
    std::string dev;
    if (!promptInput("Fail+remove from " + mdPath, "Device path to fail and remove:", "", dev) || dev.empty()) return;
    if (!confirmYesNo("Fail + remove device",
            "This marks " + dev + " faulty and removes it from " + mdPath + ".")) return;
    mdadm_.failDevice(mdPath, dev);
    auto res = mdadm_.removeDevice(mdPath, dev);
    infoDialog("Remove device", res.ok() ? "Removed." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
    refreshArrays();
}

void GuiApp::onGrowArrayClicked() {
    if (guardNotDiscovered("Grow")) return;
    std::string mdPath = selectedArrayPath();
    if (mdPath.empty()) { infoDialog("Grow", "Select an array first."); return; }
    std::string newDev;
    if (!promptInput("Grow " + mdPath, "Device to add before growing (blank to skip, if already added):", "", newDev)) return;
    if (!newDev.empty()) {
        auto res = mdadm_.addDevice(mdPath, newDev);
        if (!res.ok()) { infoDialog("Add device", "Failed:\n" + res.stdErr, Gtk::MESSAGE_ERROR); return; }
    }
    std::string countStr;
    if (!promptInput("Grow " + mdPath, "New total number of raid-devices:", "", countStr) || countStr.empty()) return;
    int count = 0;
    try { count = std::stoi(countStr); } catch (...) { infoDialog("Grow", "Invalid number.", Gtk::MESSAGE_ERROR); return; }
    if (!confirmYesNo("Grow array", "Growing " + mdPath + " to " + countStr + " devices. This can take a long time.")) return;
    auto res = mdadm_.growRaidDevices(mdPath, count);
    infoDialog("Grow", res.ok() ? "Grow started; check array detail for progress." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
    refreshArrays();
}

void GuiApp::onStopArrayClicked() {
    if (guardNotDiscovered("Stop array")) return;
    std::string mdPath = selectedArrayPath();
    if (mdPath.empty()) { infoDialog("Stop array", "Select an array first."); return; }
    if (!confirmYesNo("Stop array",
            "This stops (deactivates) " + mdPath + ". Data is not lost, but it must be re-assembled to use again.")) return;
    auto res = mdadm_.stopArray(mdPath);
    infoDialog("Stop", res.ok() ? "Stopped." : ("Failed:\n" + res.stdErr),
               res.ok() ? Gtk::MESSAGE_INFO : Gtk::MESSAGE_ERROR);
    refreshArrays();
}

void GuiApp::onArrayMountClicked() {
    if (guardNotDiscovered("Mount")) return;
    std::string mdPath = selectedArrayPath();
    if (mdPath.empty()) { infoDialog("Mount", "Select an array first."); return; }
    auto devOpt = devices_.find(mdPath);
    if (!devOpt) { infoDialog("Mount", "Could not read device info for " + mdPath, Gtk::MESSAGE_ERROR); return; }
    showMountManageDialog(*devOpt);
    refreshArrays();
}

void GuiApp::showArrayDetailDialog(const std::string& mdPath) {
    Gtk::Dialog dlg("Array: " + mdPath, *this, true);
    dlg.set_default_size(540, 420);
    dlg.add_button("Close", Gtk::RESPONSE_CLOSE);
    auto* content = dlg.get_content_area();
    content->set_spacing(8);
    content->set_border_width(8);

    Gtk::Label summary;
    summary.set_xalign(0.0);
    summary.set_line_wrap(true);
    content->pack_start(summary, Gtk::PACK_SHRINK);

    RaidMemberColumns cols;
    auto store = Gtk::ListStore::create(cols);
    Gtk::TreeView tv(store);
    tv.append_column("Member", cols.device);
    tv.append_column("State", cols.state);
    Gtk::ScrolledWindow scroll;
    scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    scroll.add(tv);
    scroll.set_vexpand(true);
    content->pack_start(scroll, Gtk::PACK_EXPAND_WIDGET);

    Gtk::Label rebuildLabel;
    rebuildLabel.set_xalign(0.0);
    Gtk::ProgressBar bar;
    content->pack_start(rebuildLabel, Gtk::PACK_SHRINK);
    content->pack_start(bar, Gtk::PACK_SHRINK);

    auto refresh = [&]() -> bool {
        auto detOpt = mdadm_.detail(mdPath);
        if (!detOpt) {
            summary.set_text("Array no longer present.");
            return true;
        }
        auto& d = *detOpt;
        std::ostringstream os;
        os << "Level: " << d.level << "   State: " << d.state << (d.degraded ? " [DEGRADED]" : "") << "\n";
        os << "Size: " << human(d.arraySizeBytes) << "   Active/Working/Failed/Spare: "
           << d.activeDevices << "/" << d.workingDevices << "/" << d.failedDevices << "/" << d.spareDevices;
        summary.set_text(os.str());
        store->clear();
        for (auto& m : d.members) {
            auto row = *store->append();
            row[cols.device] = m.device;
            row[cols.state] = m.state;
        }
        auto rebuild = mdadm_.rebuildStatus(mdPath);
        if (rebuild.active) {
            bar.show();
            rebuildLabel.show();
            bar.set_fraction(std::min(1.0, std::max(0.0, rebuild.percent / 100.0)));
            std::ostringstream rs;
            rs << rebuild.operation << ": " << std::fixed << std::setprecision(1) << rebuild.percent
               << "%  speed=" << rebuild.speed << "  eta=" << rebuild.timeRemaining;
            rebuildLabel.set_text(rs.str());
        } else {
            bar.hide();
            rebuildLabel.hide();
        }
        return true;
    };

    dlg.show_all_children();
    refresh();
    sigc::connection timer = Glib::signal_timeout().connect([&]() { return refresh(); }, 1000);

    dlg.run();
    timer.disconnect();
}

void GuiApp::showCreateArrayDialog() {
    if (!mdadm_.available()) { infoDialog("RAID", "mdadm is not installed. Run install.sh first.", Gtk::MESSAGE_ERROR); return; }
    auto disks = devices_.listDisks();
    std::string rootDisk = devices_.rootDiskName();

    Gtk::Dialog dlg("Create RAID Array", *this, true);
    dlg.set_default_size(500, 540);
    auto* content = dlg.get_content_area();
    content->set_spacing(8);
    content->set_border_width(8);

    Gtk::Label pickLabel("Select 2 or more member disks (system disk is excluded):");
    pickLabel.set_xalign(0.0);
    content->pack_start(pickLabel, Gtk::PACK_SHRINK);

    Gtk::ScrolledWindow diskScroll;
    diskScroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    diskScroll.set_size_request(-1, 160);
    Gtk::Box diskBox(Gtk::ORIENTATION_VERTICAL, 2);
    std::vector<std::pair<std::string, Gtk::CheckButton*>> diskChecks;
    for (auto& d : disks) {
        if (!rootDisk.empty() && d.kname == rootDisk) continue;
        std::ostringstream label;
        label << d.path << "  " << human(d.sizeBytes) << "  " << (d.model.empty() ? "" : d.model)
              << (d.mountpoint.empty() ? "" : "  [mounted at " + d.mountpoint + "]");
        auto* cb = Gtk::manage(new Gtk::CheckButton(label.str()));
        diskBox.pack_start(*cb, Gtk::PACK_SHRINK);
        diskChecks.emplace_back(d.path, cb);
    }
    diskScroll.add(diskBox);
    content->pack_start(diskScroll, Gtk::PACK_SHRINK);

    if (diskChecks.size() < 2) {
        infoDialog("Create array", "Need at least 2 non-system disks to create a RAID array.", Gtk::MESSAGE_ERROR);
        return;
    }

    Gtk::Label levelLabel("RAID level:");
    levelLabel.set_xalign(0.0);
    content->pack_start(levelLabel, Gtk::PACK_SHRINK);
    Gtk::ComboBoxText levelCombo;
    levelCombo.append("0", "0 (striping, no redundancy)");
    levelCombo.append("1", "1 (mirror)");
    levelCombo.append("5", "5 (parity, N-1 usable)");
    levelCombo.append("6", "6 (dual parity, N-2 usable)");
    levelCombo.append("10", "10 (mirrored stripes)");
    levelCombo.set_active_id("1");
    content->pack_start(levelCombo, Gtk::PACK_SHRINK);

    Gtk::Label nameLabel("New array device name:");
    nameLabel.set_xalign(0.0);
    content->pack_start(nameLabel, Gtk::PACK_SHRINK);
    Gtk::Entry nameEntry;
    nameEntry.set_text("/dev/md0");
    content->pack_start(nameEntry, Gtk::PACK_SHRINK);

    Gtk::Label warnLabel;
    warnLabel.set_xalign(0.0);
    warnLabel.set_line_wrap(true);
    warnLabel.set_markup("<b>Warning:</b> creating the array DESTROYS ALL DATA on every selected disk.");
    content->pack_start(warnLabel, Gtk::PACK_SHRINK);

    Gtk::Label confirmLabel("Type \"yes\" to enable the Create button:");
    confirmLabel.set_xalign(0.0);
    content->pack_start(confirmLabel, Gtk::PACK_SHRINK);
    Gtk::Entry confirmEntry;
    content->pack_start(confirmEntry, Gtk::PACK_SHRINK);

    dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    auto* createBtn = dlg.add_button("Create", Gtk::RESPONSE_OK);
    createBtn->set_sensitive(false);
    confirmEntry.signal_changed().connect([&confirmEntry, createBtn]() {
        createBtn->set_sensitive(confirmEntry.get_text() == "yes");
    });

    dlg.show_all_children();
    int resp = dlg.run();
    if (resp != Gtk::RESPONSE_OK) return;

    std::vector<std::string> members;
    for (auto& kv : diskChecks)
        if (kv.second->get_active()) members.push_back(kv.first);
    if (members.size() < 2) { infoDialog("Create array", "Select at least 2 disks.", Gtk::MESSAGE_ERROR); return; }
    std::string level = levelCombo.get_active_id();
    if (level.empty()) level = "1";
    std::string mdPath = nameEntry.get_text();
    if (mdPath.empty()) { infoDialog("Create array", "Enter a device name.", Gtk::MESSAGE_ERROR); return; }

    runWithProgressDialog("Creating " + mdPath, [&](std::atomic<bool>&, const std::function<void(double, const std::string&)>& update) {
        update(10, "running mdadm --create...");
        auto res = mdadm_.createArray(mdPath, level, members);
        update(100, res.ok() ? "done" : ("failed: " + res.stdErr));
    });
    infoDialog("Create array", "mdadm --create issued for " + mdPath + ".\nCheck the RAID Arrays tab for sync progress.");
}

void GuiApp::showReplaceDriveDialog(const std::string& mdPath) {
    auto detOpt = mdadm_.detail(mdPath);
    if (!detOpt) { infoDialog("Replace drive", "Could not read array detail for " + mdPath, Gtk::MESSAGE_ERROR); return; }

    Gtk::Dialog dlg("Replace drive in " + mdPath, *this, true);
    dlg.set_default_size(500, 380);
    auto* content = dlg.get_content_area();
    content->set_spacing(8);
    content->set_border_width(8);

    Gtk::Label pickLabel("Select the failing/failed member to replace:");
    pickLabel.set_xalign(0.0);
    content->pack_start(pickLabel, Gtk::PACK_SHRINK);

    RaidMemberColumns cols;
    auto store = Gtk::ListStore::create(cols);
    for (auto& m : detOpt->members) {
        auto report = smart_.query(m.device);
        bool risk = smart_.isAtRisk(report);
        auto row = *store->append();
        row[cols.device] = m.device;
        row[cols.state] = m.state + (risk ? "  [SMART AT-RISK]" : "");
    }
    Gtk::TreeView tv(store);
    tv.append_column("Member", cols.device);
    tv.append_column("State", cols.state);
    Gtk::ScrolledWindow scroll;
    scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    scroll.set_size_request(-1, 140);
    scroll.add(tv);
    content->pack_start(scroll, Gtk::PACK_SHRINK);

    Gtk::Button locateBtn("Locate selected drive (blink activity light)");
    content->pack_start(locateBtn, Gtk::PACK_SHRINK);
    locateBtn.signal_clicked().connect([&, this]() {
        auto iter = tv.get_selection()->get_selected();
        if (!iter) { infoDialog("Locate", "Select a member first."); return; }
        Glib::ustring dev = (*iter)[cols.device];
        auto method = locate_.bestMethodFor(dev);
        if (method == LocateMethod::None) { infoDialog("Locate", "No way to identify this drive was found."); return; }
        locate_.start(dev, 15);
        infoDialog("Locating", "Watch the drive bay for 15 seconds of activity on " + dev + ".");
    });

    Gtk::Label newDevLabel("Path of the REPLACEMENT drive/partition (already inserted):");
    newDevLabel.set_xalign(0.0);
    content->pack_start(newDevLabel, Gtk::PACK_SHRINK);
    Gtk::Entry newDevEntry;
    content->pack_start(newDevEntry, Gtk::PACK_SHRINK);

    dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("Replace", Gtk::RESPONSE_OK);
    dlg.show_all_children();
    int resp = dlg.run();
    if (resp != Gtk::RESPONSE_OK) return;

    auto iter = tv.get_selection()->get_selected();
    if (!iter) { infoDialog("Replace drive", "Select a member to replace."); return; }
    Glib::ustring oldDev = (*iter)[cols.device];
    std::string newDev = newDevEntry.get_text();
    if (newDev.empty()) { infoDialog("Replace drive", "Enter the replacement device path."); return; }
    if (!confirmYesNo("Replace drive",
            "Replacing " + oldDev + " with " + newDev + " in " + mdPath + ".\n"
            "Any data currently on " + newDev + " will be destroyed.")) return;

    auto res = mdadm_.replaceDevice(mdPath, oldDev, newDev);
    if (!res.ok()) { infoDialog("Replace drive", "Failed:\n" + res.stdErr, Gtk::MESSAGE_ERROR); return; }

    watchRebuild(mdPath, "Rebuilding " + mdPath);
}

void GuiApp::runWithProgressDialog(const std::string& title,
        const std::function<void(std::atomic<bool>& cancel,
                                  const std::function<void(double, const std::string&)>& update)>& work) {
    ProgressDialog dlg(*this, title);
    std::thread worker([&]() {
        work(dlg.cancelFlag(), [&](double p, const std::string& s) { dlg.update(p, s); });
        dlg.finish();
    });
    dlg.run();
    if (worker.joinable()) worker.join();
}

void GuiApp::watchRebuild(const std::string& mdPath, const std::string& title) {
    runWithProgressDialog(title, [&](std::atomic<bool>& cancel, const std::function<void(double, const std::string&)>& update) {
        while (!cancel.load()) {
            auto rb = mdadm_.rebuildStatus(mdPath);
            if (!rb.active) { update(100, "rebuild complete or not reported by mdstat"); return; }
            std::ostringstream os;
            os << rb.operation << "  speed=" << rb.speed << "  eta=" << rb.timeRemaining;
            update(rb.percent, os.str());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        update(0, "monitoring cancelled (rebuild continues in the background)");
    });
}

// ============================================================= Firmware tab

void GuiApp::buildFirmwareTab() {
    firmwareTab_.set_border_width(6);
    firmwareAvailable_ = firmware_.fwupdAvailable();
    if (!firmwareAvailable_) {
        firmwareUnavailableLabel_.set_line_wrap(true);
        firmwareUnavailableLabel_.set_text(
            "fwupdmgr is not installed or not available on this system.\n"
            "Firmware update management is disabled. Install fwupd (e.g. via install.sh) "
            "to enable this tab.");
        firmwareTab_.pack_start(firmwareUnavailableLabel_, Gtk::PACK_SHRINK);
        notebook_.append_page(firmwareTab_, "Firmware");
        return;
    }

    firmwareStore_ = Gtk::ListStore::create(firmwareCols_);
    firmwareView_.set_model(firmwareStore_);
    firmwareView_.append_column("Device", firmwareCols_.devicePath);
    firmwareView_.append_column("Vendor / Name", firmwareCols_.vendorName);
    firmwareView_.append_column("Current", firmwareCols_.current);
    firmwareView_.append_column("Latest", firmwareCols_.latest);
    firmwareView_.append_column("Update available", firmwareCols_.updateAvailable);
    firmwareView_.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &GuiApp::onFirmwareSelectionChanged));

    firmwareScroll_.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    firmwareScroll_.add(firmwareView_);
    firmwareScroll_.set_vexpand(true);
    firmwareTab_.pack_start(firmwareScroll_, Gtk::PACK_EXPAND_WIDGET);

    firmwareButtonBox_.pack_start(firmwareRefreshBtn_, Gtk::PACK_SHRINK);
    firmwareButtonBox_.pack_start(firmwareUpdateBtn_, Gtk::PACK_SHRINK);
    firmwareTab_.pack_start(firmwareButtonBox_, Gtk::PACK_SHRINK);
    firmwareUpdateBtn_.set_sensitive(false);

    firmwareRefreshBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onFirmwareRefreshClicked));
    firmwareUpdateBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onFirmwareUpdateClicked));

    notebook_.append_page(firmwareTab_, "Firmware");
    refreshFirmware(false);
}

void GuiApp::refreshFirmware(bool refreshFromNetwork) {
    if (!firmwareAvailable_) return;
    if (refreshFromNetwork) {
        runWithProgressDialog("Refreshing firmware metadata",
            [&](std::atomic<bool>&, const std::function<void(double, const std::string&)>& update) {
                update(50, "fwupdmgr refresh...");
                auto res = firmware_.refreshMetadata();
                update(100, res.ok() ? "done" : "failed (continuing with cached metadata)");
            });
    }
    auto disks = devices_.listDisks();
    std::vector<std::string> paths;
    for (auto& d : disks) paths.push_back(d.path);
    firmwareInfos_ = firmware_.listStorageDevices(paths);

    firmwareStore_->clear();
    for (size_t i = 0; i < firmwareInfos_.size(); ++i) {
        auto& f = firmwareInfos_[i];
        auto row = *firmwareStore_->append();
        row[firmwareCols_.index] = static_cast<int>(i);
        row[firmwareCols_.devicePath] = f.devicePath.empty() ? "(unmatched device)" : f.devicePath;
        row[firmwareCols_.vendorName] = f.vendor + " " + f.name;
        row[firmwareCols_.current] = f.currentVersion;
        row[firmwareCols_.latest] = f.latestVersion;
        row[firmwareCols_.updateAvailable] = f.updateAvailable ? "Yes" : "No";
    }
    firmwareUpdateBtn_.set_sensitive(false);
    if (firmwareInfos_.empty()) {
        infoDialog("Firmware", "No fwupd-manageable storage devices found on this system.");
    }
}

void GuiApp::onFirmwareRefreshClicked() {
    refreshFirmware(true);
}

void GuiApp::onFirmwareSelectionChanged() {
    auto iter = firmwareView_.get_selection()->get_selected();
    if (!iter) { firmwareUpdateBtn_.set_sensitive(false); return; }
    int idx = (*iter)[firmwareCols_.index];
    bool avail = idx >= 0 && idx < (int)firmwareInfos_.size() && firmwareInfos_[idx].updateAvailable;
    firmwareUpdateBtn_.set_sensitive(avail);
}

void GuiApp::onFirmwareUpdateClicked() {
    auto iter = firmwareView_.get_selection()->get_selected();
    if (!iter) return;
    int idx = (*iter)[firmwareCols_.index];
    if (idx < 0 || idx >= (int)firmwareInfos_.size()) return;
    auto& f = firmwareInfos_[idx];
    if (!f.updateAvailable) { infoDialog("Firmware", "No update available for " + f.name + "."); return; }
    std::ostringstream warn;
    warn << "Updating firmware on " << f.name << " (" << f.currentVersion << " -> " << f.latestVersion << ").\n"
         << "Do not power off or remove this drive during the update -- an\n"
         << "interrupted firmware write can permanently damage the drive.";
    if (!confirmYesNo("Firmware update", warn.str())) return;

    runWithProgressDialog("Updating firmware on " + f.name,
        [&](std::atomic<bool>&, const std::function<void(double, const std::string&)>& update) {
            update(20, "fwupdmgr update...");
            auto res = firmware_.applyUpdate(f.fwupdDeviceId);
            update(100, res.ok() ? "done" : ("failed: " + res.stdErr));
        });
    infoDialog("Firmware update", "Update issued (a reboot may be required). Check status with Refresh metadata.");
    refreshFirmware(false);
}

// ============================================================= Recovery tab

void GuiApp::buildRecoveryTab() {
    recoveryTab_.set_border_width(8);

    ddrescueGrid_.set_row_spacing(6);
    ddrescueGrid_.set_column_spacing(6);
    ddrescueGrid_.set_border_width(8);

    auto* srcLabel = Gtk::manage(new Gtk::Label("Source device (disk, partition, or /dev/mdX):"));
    srcLabel->set_xalign(0.0);
    ddrescueGrid_.attach(*srcLabel, 0, 0, 1, 1);
    ddrescueSourceEntry_.set_hexpand(true);
    ddrescueGrid_.attach(ddrescueSourceEntry_, 1, 0, 2, 1);

    auto* destLabel = Gtk::manage(new Gtk::Label("Destination image file:"));
    destLabel->set_xalign(0.0);
    ddrescueGrid_.attach(*destLabel, 0, 1, 1, 1);
    ddrescueDestEntry_.set_hexpand(true);
    ddrescueGrid_.attach(ddrescueDestEntry_, 1, 1, 1, 1);
    ddrescueGrid_.attach(ddrescueBrowseBtn_, 2, 1, 1, 1);

    auto* mapLabel = Gtk::manage(new Gtk::Label("Mapfile path (for resuming if interrupted):"));
    mapLabel->set_xalign(0.0);
    ddrescueGrid_.attach(*mapLabel, 0, 2, 1, 1);
    ddrescueMapEntry_.set_hexpand(true);
    ddrescueGrid_.attach(ddrescueMapEntry_, 1, 2, 2, 1);

    ddrescueGrid_.attach(ddrescueStartBtn_, 1, 3, 1, 1);

    ddrescueFrame_.add(ddrescueGrid_);
    recoveryTab_.pack_start(ddrescueFrame_, Gtk::PACK_SHRINK);

    testdiskBox_.set_border_width(8);
    testdiskBox_.pack_start(launchTestdiskBtn_, Gtk::PACK_SHRINK);
    testdiskBox_.pack_start(launchPhotorecBtn_, Gtk::PACK_SHRINK);
    testdiskFrame_.add(testdiskBox_);
    recoveryTab_.pack_start(testdiskFrame_, Gtk::PACK_SHRINK);

    // Keep the mapfile suggestion in sync with the destination path, mirroring
    // the TUI's default of "<dest>.mapfile".
    ddrescueDestEntry_.signal_changed().connect([this]() {
        std::string dest = ddrescueDestEntry_.get_text();
        if (!dest.empty()) ddrescueMapEntry_.set_text(dest + ".mapfile");
    });

    ddrescueBrowseBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDdrescueBrowseClicked));
    ddrescueStartBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onDdrescueStartClicked));
    launchTestdiskBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onLaunchTestdiskClicked));
    launchPhotorecBtn_.signal_clicked().connect(sigc::mem_fun(*this, &GuiApp::onLaunchPhotorecClicked));

    notebook_.append_page(recoveryTab_, "Data Recovery");
}

void GuiApp::onDdrescueBrowseClicked() {
    // Gtk::FileChooserButton officially only supports ACTION_OPEN /
    // ACTION_SELECT_FOLDER, not ACTION_SAVE, so we use a FileChooserDialog in
    // save mode directly (Browse... button next to a plain Entry) instead.
    Gtk::FileChooserDialog chooser(*this, "Select destination image file", Gtk::FILE_CHOOSER_ACTION_SAVE);
    chooser.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    chooser.add_button("Select", Gtk::RESPONSE_OK);
    chooser.set_do_overwrite_confirmation(true);
    if (chooser.run() == Gtk::RESPONSE_OK) {
        ddrescueDestEntry_.set_text(chooser.get_filename());
    }
}

void GuiApp::onDdrescueStartClicked() {
    if (!recovery_.ddrescueAvailable()) { infoDialog("Recovery", "ddrescue is not installed. Run install.sh first.", Gtk::MESSAGE_ERROR); return; }
    std::string source = ddrescueSourceEntry_.get_text();
    std::string dest = ddrescueDestEntry_.get_text();
    std::string mapFile = ddrescueMapEntry_.get_text();
    if (source.empty() || dest.empty()) { infoDialog("Recovery", "Source device and destination image path are required."); return; }
    if (mapFile.empty()) mapFile = dest + ".mapfile";
    if (!confirmYesNo("Image with ddrescue",
            "Imaging " + source + " -> " + dest + ".\nThis reads " + source + " but never writes to it.")) return;

    runWithProgressDialog("ddrescue " + source,
        [&, source, dest, mapFile](std::atomic<bool>& cancel, const std::function<void(double, const std::string&)>& update) {
            recovery_.rescue(source, dest, mapFile, [&](const RescueProgress& p) {
                std::ostringstream os;
                os << p.currentStatus << "  rescued=" << human(p.rescuedBytes) << "/" << human(p.totalBytes)
                   << "  errors=" << p.errorCount;
                update(p.percent, os.str());
            }, &cancel);
        });
    infoDialog("ddrescue", "Finished (or cancelled). Mapfile kept at:\n" + mapFile + "\nRe-run with the same mapfile to resume.");
}

// testdisk/photorec are full-screen (n)curses programs that need a real
// controlling terminal; we can't embed them in a GTK widget, so -- per the
// task brief -- we spawn them in the user's terminal emulator instead. This
// is a known, documented limitation of this first GUI version.
void GuiApp::spawnInTerminal(const std::vector<std::string>& innerArgv) {
    std::string term;
    if (commandExists("x-terminal-emulator")) term = "x-terminal-emulator";
    else if (commandExists("xterm")) term = "xterm";
    else { infoDialog("Launch", "No terminal emulator found (tried x-terminal-emulator, xterm).", Gtk::MESSAGE_ERROR); return; }

    std::vector<std::string> argv = {term, "-e"};
    for (auto& a : innerArgv) argv.push_back(a);

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    GError* error = nullptr;
    gboolean ok = g_spawn_async(nullptr, cargv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error);
    if (!ok) {
        std::string msg = (error && error->message) ? error->message : "unknown error";
        if (error) g_error_free(error);
        infoDialog("Launch", "Failed to launch terminal: " + msg, Gtk::MESSAGE_ERROR);
    }
}

void GuiApp::onLaunchTestdiskClicked() {
    if (!recovery_.testdiskAvailable()) { infoDialog("Recovery", "testdisk is not installed. Run install.sh first.", Gtk::MESSAGE_ERROR); return; }
    std::string target;
    if (!promptInput("Launch testdisk", "Target device (blank = let testdisk choose):", "", target)) return;
    std::vector<std::string> argv = {"testdisk"};
    if (!target.empty()) argv.push_back(target);
    spawnInTerminal(argv);
}

void GuiApp::onLaunchPhotorecClicked() {
    if (!recovery_.testdiskAvailable()) { infoDialog("Recovery", "photorec is not installed. Run install.sh first.", Gtk::MESSAGE_ERROR); return; }
    std::string target;
    if (!promptInput("Launch photorec", "Target device (blank = let photorec choose):", "", target)) return;
    std::vector<std::string> argv = {"photorec"};
    if (!target.empty()) argv.push_back(target);
    spawnInTerminal(argv);
}

// ========================================================= generic dialogs

bool GuiApp::confirmYesNo(const std::string& title, const std::string& message, Gtk::MessageType type) {
    Gtk::MessageDialog dlg(*this, message, false, type, Gtk::BUTTONS_YES_NO, true);
    dlg.set_title(title);
    int resp = dlg.run();
    return resp == Gtk::RESPONSE_YES;
}

void GuiApp::infoDialog(const std::string& title, const std::string& message, Gtk::MessageType type) {
    Gtk::MessageDialog dlg(*this, message, false, type, Gtk::BUTTONS_OK, true);
    dlg.set_title(title);
    dlg.run();
}

bool GuiApp::promptInput(const std::string& title, const std::string& label, const std::string& initial, std::string& out) {
    Gtk::Dialog dlg(title, *this, true);
    dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("OK", Gtk::RESPONSE_OK);
    dlg.set_default_response(Gtk::RESPONSE_OK);

    auto* content = dlg.get_content_area();
    content->set_spacing(6);
    content->set_border_width(8);
    Gtk::Label lbl(label);
    lbl.set_xalign(0.0);
    lbl.set_line_wrap(true);
    Gtk::Entry entry;
    entry.set_text(initial);
    entry.set_activates_default(true);
    content->pack_start(lbl, Gtk::PACK_SHRINK);
    content->pack_start(entry, Gtk::PACK_SHRINK);

    dlg.show_all_children();
    int resp = dlg.run();
    if (resp == Gtk::RESPONSE_OK) {
        out = entry.get_text();
        return true;
    }
    return false;
}

} // namespace dm
