// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/updater/updater_service.h"
#include "citron/uisettings.h"
#include "common/logging/log.h"
#include "common/fs/path_util.h"
#include "common/scm_rev.h"

#include <QApplication>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QCoreApplication>
#include <QSslSocket>
#include <QProcess>
#include <QSettings>

#ifdef CITRON_ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include <fstream>
#include <regex>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace Updater {

const std::string STABLE_UPDATE_URL = "https://git.citron-emu.org/api/v1/repos/Citron/Emulator/releases";
const std::string NIGHTLY_UPDATE_URL = "https://api.github.com/repos/CollectingW/Citron-CI/releases";

std::string ExtractCommitHash(const std::string& version_string) {
    std::regex re("([0-9a-fA-F]{7,40})");
    std::smatch match;
    if (std::regex_search(version_string, match, re)) {
        return match[0].str();
    }
    return "";
}

UpdaterService::UpdaterService(QObject* parent) : QObject(parent) {
    network_manager = std::make_unique<QNetworkAccessManager>(this);
    InitializeSSL();

    app_directory = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    temp_download_path = std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::TempLocation).toStdString()) / "citron_updater";
    backup_path = app_directory / BACKUP_DIRECTORY;

    if (!std::filesystem::exists(temp_download_path)) {
        std::filesystem::create_directories(temp_download_path);
    }
    EnsureDirectoryExists(backup_path);
}

UpdaterService::~UpdaterService() {
    if (current_reply) {
        current_reply->abort();
        current_reply->deleteLater();
    }
    CleanupFiles();
}

void UpdaterService::InitializeSSL() {
    if (!QSslSocket::supportsSsl()) return;
    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
    sslConfig.setProtocol(QSsl::SecureProtocols);
    QSslConfiguration::setDefaultConfiguration(sslConfig);
}

void UpdaterService::CheckForUpdates(const QString& override_channel) {
    if (update_in_progress.load()) return;

    QString channel = override_channel.isEmpty() ?
        QSettings().value(QStringLiteral("updater/channel"), QStringLiteral("Nightly")).toString() : override_channel;

    std::string update_url = (channel.toLower() == QStringLiteral("nightly")) ? NIGHTLY_UPDATE_URL : STABLE_UPDATE_URL;

    QNetworkRequest request{QUrl(QString::fromStdString(update_url))};
    request.setRawHeader("User-Agent", QByteArrayLiteral("Citron-Updater/1.0"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    current_reply = network_manager->get(request);
    connect(current_reply, &QNetworkReply::finished, this, [this, channel]() {
        if (!current_reply) return;
        if (current_reply->error() == QNetworkReply::NoError) {
            ParseUpdateResponse(current_reply->readAll(), channel);
        } else {
            emit UpdateError(current_reply->errorString());
        }
        current_reply->deleteLater();
        current_reply = nullptr;
    });
}

void UpdaterService::DownloadAndInstallUpdate(const std::string& download_url) {
    if (update_in_progress.load()) return;
    update_in_progress.store(true);
    cancel_requested.store(false);

#ifdef _WIN32
    if (!CreateBackup()) {
        emit UpdateCompleted(UpdateResult::PermissionError, QStringLiteral("Failed to create backup of citron.exe"));
        update_in_progress.store(false);
        return;
    }
#endif

    QNetworkRequest request{QUrl(QString::fromStdString(download_url))};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    current_reply = network_manager->get(request);
    connect(current_reply, &QNetworkReply::downloadProgress, this, &UpdaterService::OnDownloadProgress);
    connect(current_reply, &QNetworkReply::finished, this, &UpdaterService::OnDownloadFinished);
}

void UpdaterService::CancelUpdate() {
    if (!update_in_progress.load()) return;
    cancel_requested.store(true);
    if (current_reply) current_reply->abort();
    update_in_progress.store(false);
}

std::string UpdaterService::GetCurrentVersion(const QString& channel) const {
    QString active = channel.isEmpty() ?
        QSettings().value(QStringLiteral("updater/channel"), QStringLiteral("Nightly")).toString() : channel;

    if (active.toLower() == QStringLiteral("nightly")) {
        return Common::g_citron_hash;
    }
    return Common::g_citron_version;
}

bool UpdaterService::IsUpdateInProgress() const {
    return update_in_progress.load();
}

void UpdaterService::OnDownloadFinished() {
    if (cancel_requested.load() || !current_reply) {
        update_in_progress.store(false);
        return;
    }

    if (current_reply->error() != QNetworkReply::NoError) {
        emit UpdateError(current_reply->errorString());
        update_in_progress.store(false);
        return;
    }

    QByteArray data = current_reply->readAll();

#if defined(_WIN32)
    std::filesystem::path zip_path = temp_download_path / "citron_update.zip";
    QFile f(QString::fromStdString(zip_path.string()));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(data);
        f.close();
    }

    // Process extraction on a slight delay to ensure file handles are clear
    QTimer::singleShot(100, this, [this, zip_path]() {
        std::filesystem::path extract_path = temp_download_path / "extracted";

        if (!ExtractArchive(zip_path, extract_path)) {
            RestoreBackup();
            emit UpdateCompleted(UpdateResult::ExtractionError, QStringLiteral("Failed to extract update. Ensure you have permissions."));
            update_in_progress.store(false);
            return;
        }

        if (!InstallUpdate(extract_path)) {
            RestoreBackup();
            emit UpdateCompleted(UpdateResult::Failed, QStringLiteral("Failed to stage update files."));
            update_in_progress.store(false);
            return;
        }

        emit UpdateCompleted(UpdateResult::Success, QStringLiteral("Update staged successfully."));
        update_in_progress.store(false);
    });

#elif defined(__linux__)
    const char* appimage_env = qgetenv("APPIMAGE").constData();
    if (!appimage_env || strlen(appimage_env) == 0) {
        emit UpdateError(QStringLiteral("Not running from an AppImage. Manual update required."));
        update_in_progress.store(false);
        return;
    }

    std::filesystem::path original_path = appimage_env;

    // Backup logic
    if (UISettings::values.updater_enable_backups.GetValue()) {
        std::filesystem::path backup_dir = UISettings::values.updater_backup_path.GetValue().empty() ?
            (original_path.parent_path() / "backup") : std::filesystem::path(UISettings::values.updater_backup_path.GetValue());

        std::error_code ec;
        std::filesystem::create_directories(backup_dir, ec);
        std::filesystem::copy_file(original_path, backup_dir / ("citron-backup-" + GetCurrentVersion() + ".AppImage"),
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }

    std::filesystem::path new_path = original_path.string() + ".new";
    QFile n_file(QString::fromStdString(new_path.string()));
    if (n_file.open(QIODevice::WriteOnly)) {
        n_file.write(data);
        n_file.close();

        // chmod +x
        n_file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|
                              QFileDevice::ReadGroup|QFileDevice::ExeGroup|
                              QFileDevice::ReadOther|QFileDevice::ExeOther);

        std::error_code ec;
        std::filesystem::rename(new_path, original_path, ec);
        if (ec) {
             emit UpdateError(QString::fromStdString("Failed to replace AppImage: " + ec.message()));
             update_in_progress.store(false);
             return;
        }
    }

    emit UpdateCompleted(UpdateResult::Success, QStringLiteral("Success"));
    update_in_progress.store(false);
#endif
}

void UpdaterService::ParseUpdateResponse(const QByteArray& response, const QString& channel) {
    QJsonDocument doc = QJsonDocument::fromJson(response);
    if (!doc.isArray()) return;

    std::string current_variant = Common::g_citron_variant;

    for (const QJsonValue& rel_val : doc.array()) {
        QJsonObject rel_obj = rel_val.toObject();

        std::string latest_v;
        if (channel.toLower() == QStringLiteral("stable")) {
            latest_v = rel_obj.value(QStringLiteral("tag_name")).toString().toStdString();
        } else {
            // Nightly names are usually "Nightly Build - [hash]"
            latest_v = ExtractCommitHash(rel_obj.value(QStringLiteral("name")).toString().toStdString());
        }

        if (latest_v.empty()) continue;

        UpdateInfo info;
        info.version = latest_v;
        info.changelog = rel_obj.value(QStringLiteral("body")).toString().toStdString();
        info.release_date = rel_obj.value(QStringLiteral("published_at")).toString().toStdString();

        QJsonArray assets = rel_obj.value(QStringLiteral("assets")).toArray();
        std::vector<DownloadOption> os_valid;
        std::vector<DownloadOption> variant_matched;

        for (const QJsonValue& asset_val : assets) {
            QJsonObject asset_obj = asset_val.toObject();
            QString name = asset_obj.value(QStringLiteral("name")).toString();
            DownloadOption opt;
            opt.name = name.toStdString();
            opt.url = asset_obj.value(QStringLiteral("browser_download_url")).toString().toStdString();

#if defined(__linux__)
            if (!name.endsWith(QStringLiteral(".AppImage"), Qt::CaseInsensitive)) continue;
            os_valid.push_back(opt);

            // Architecture matching
            if (current_variant.find("aarch64") != std::string::npos && name.contains(QStringLiteral("aarch64"), Qt::CaseInsensitive)) {
                variant_matched.push_back(opt);
            } else if (current_variant.find("v3") != std::string::npos && name.contains(QStringLiteral("v3"), Qt::CaseInsensitive)) {
                variant_matched.push_back(opt);
            } else if (!name.contains(QStringLiteral("v3"), Qt::CaseInsensitive) && !name.contains(QStringLiteral("aarch64"), Qt::CaseInsensitive)) {
                // Fallback for generic x86_64
                if (current_variant.find("x86_64") != std::string::npos && current_variant.find("v3") == std::string::npos) {
                    variant_matched.push_back(opt);
                }
            }
#elif defined(_WIN32)
            if (!name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive) || name.contains(QStringLiteral("PGO"), Qt::CaseInsensitive)) continue;
            os_valid.push_back(opt);

            // Windows Variant Matching (v3 vs generic)
            if (current_variant.find("v3") != std::string::npos && name.contains(QStringLiteral("v3"), Qt::CaseInsensitive)) {
                variant_matched.push_back(opt);
            } else if (current_variant.find("v3") == std::string::npos && !name.contains(QStringLiteral("v3"), Qt::CaseInsensitive)) {
                variant_matched.push_back(opt);
            }
#endif
        }

        // Priority: Match Variant -> Any OS Valid -> Fail
        info.download_options = variant_matched.empty() ? os_valid : variant_matched;

        if (!info.download_options.empty()) {
            std::string current_v = GetCurrentVersion(channel);
            info.is_newer_version = (current_v != info.version);

            current_update_info = info;
            emit UpdateCheckCompleted(info.is_newer_version, info);
            return;
        }
    }
}

#ifdef _WIN32
bool UpdaterService::ExtractArchive(const std::filesystem::path& a_p, const std::filesystem::path& e_p) {
#ifdef CITRON_ENABLE_LIBARCHIVE
    struct archive* a = archive_read_new();
    struct archive* ext = archive_write_disk_new();
    if (!a || !ext) return false;

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL);

    if (archive_read_open_filename(a, a_p.string().c_str(), 10240) != ARCHIVE_OK) return false;

    std::filesystem::create_directories(e_p);
    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::filesystem::path full_path = e_p / archive_entry_pathname(entry);
        archive_entry_set_pathname(entry, full_path.string().c_str());
        archive_write_header(ext, entry);

        const void* buff; size_t sz; la_int64_t off;
        while (archive_read_data_block(a, &buff, &sz, &off) == ARCHIVE_OK) {
            archive_write_data_block(ext, buff, sz, off);
        }
        archive_write_finish_entry(ext);
    }
    archive_read_free(a);
    archive_write_free(ext);
    return true;
#else
    // Windows PowerShell Fallback if LibArchive is missing
    QString cmd = QString("powershell -Command \"Expand-Archive -Path '%1' -DestinationPath '%2' -Force\"")
                  .arg(QString::fromStdString(a_p.string()))
                  .arg(QString::fromStdString(e_p.string()));
    return QProcess::execute(cmd) == 0;
#endif
}

bool UpdaterService::InstallUpdate(const std::filesystem::path& u_p) {
    try {
        std::filesystem::path staging = app_directory / "update_staging";
        std::filesystem::create_directories(staging);

        // Clean staging area first
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);
        std::filesystem::create_directories(staging);

        for (const auto& entry : std::filesystem::recursive_directory_iterator(u_p)) {
            if (entry.is_regular_file()) {
                std::filesystem::path rel = std::filesystem::relative(entry.path(), u_p);
                std::filesystem::path dest = staging / rel;
                std::filesystem::create_directories(dest.parent_path());
                std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
            }
        }
        return CreateUpdateHelperScript(staging);
    } catch (...) { return false; }
}

bool UpdaterService::CreateBackup() {
    try {
        std::filesystem::path b_file = backup_path / "citron.exe.bak";
        std::filesystem::copy_file(app_directory / "citron.exe", b_file, std::filesystem::copy_options::overwrite_existing);
        return true;
    } catch (...) { return false; }
}

bool UpdaterService::RestoreBackup() {
    try {
        std::filesystem::path b_file = backup_path / "citron.exe.bak";
        if (std::filesystem::exists(b_file)) {
            std::filesystem::copy_file(b_file, app_directory / "citron.exe", std::filesystem::copy_options::overwrite_existing);
            return true;
        }
    } catch (...) {}
    return false;
}

bool UpdaterService::CreateUpdateHelperScript(const std::filesystem::path& staging_path) {
    try {
        std::ofstream script(staging_path / "apply_update.bat");
        if (!script.is_open()) return false;

        std::string staging_path_str = staging_path.string();
        std::string app_path_str = app_directory.string();
        std::string exe_path_str = (app_directory / "citron.exe").string();

        // Ensure Windows-style backslashes for the batch file
        for (auto& ch : staging_path_str) if (ch == '/') ch = '\\';
        for (auto& ch : app_path_str) if (ch == '/') ch = '\\';
        for (auto& ch : exe_path_str) if (ch == '/') ch = '\\';

        script << "@echo off\n";
        script << "setlocal enabledelayedexpansion\n";
        script << "title Citron Auto-Updater\n";
        script << "color 0B\n";
        script << "echo =======================================\n";
        script << "echo        Citron Emulator Updater       \n";
        script << "echo =======================================\n\n";

        script << "echo Waiting for Citron to close...\n";

        // Wait for process to exit with a 60s timeout
        script << "set /a wait_count=0\n";
        script << ":wait_loop\n";
        script << "tasklist /FI \"IMAGENAME eq citron.exe\" | find /I \"citron.exe\" >nul 2>&1\n";
        script << "if not errorlevel 1 (\n";
        script << "    set /a wait_count+=1\n";
        script << "    if !wait_count! gtr 60 (\n";
        script << "        echo [WARNING] Citron is taking a long time to close. Attempting to proceed...\n";
        script << "        goto wait_done\n";
        script << "    )\n";
        script << "    timeout /t 1 /nobreak >nul\n";
        script << "    goto wait_loop\n";
        script << ")\n";
        script << ":wait_done\n";
        script << "timeout /t 2 /nobreak >nul\n";

        // Permission fix
        script << "echo Preparing permissions...\n";
        script << "attrib -R \"" << app_path_str << "\\*.*\" /S /D >nul 2>&1\n";

        // Reliable Robocopy with error checking
        script << "echo Applying update files...\n";
        script << "set /a copy_retries=0\n";
        script << ":copy_loop\n";
        // /E: recursive, /IS: include same, /IT: include tweaked, /R:3: retries, /W:1: wait between retries
        script << "robocopy \"" << staging_path_str << "\" \"" << app_path_str << "\" /E /IS /IT /R:3 /W:1 /NP /NFL /NDL >nul 2>&1\n";
        script << "set /a robocopy_exit=!errorlevel!\n";

        // Robocopy success is anything below 8
        script << "if !robocopy_exit! geq 8 (\n";
        script << "    set /a copy_retries+=1\n";
        script << "    if !copy_retries! lss 3 (\n";
        script << "        echo [RETRY] Copy failed (Error !robocopy_exit!). Retrying in 2s...\n";
        script << "        timeout /t 2 /nobreak >nul\n";
        script << "        goto copy_loop\n";
        script << "    ) else (\n";
        script << "        echo [ERROR] Update failed to copy files. Error code: !robocopy_exit!\n";
        script << "        echo Please ensure no other programs are using Citron files.\n";
        script << "        pause\n";
        script << "        exit /b 1\n";
        script << "    )\n";
        script << ")\n\n";

        // Post-install verification
        script << "if not exist \"" << exe_path_str << "\" (\n";
        script << "    echo [ERROR] Critical file citron.exe is missing after update!\n";
        script << "    pause\n";
        script << "    exit /b 1\n";
        script << ")\n\n";

        script << "echo Update applied successfully!\n";
        script << "echo Restarting Citron...\n";
        script << "start \"\" \"" << exe_path_str << "\"\n\n";

        // Cleanup with retry loop
        script << "echo Cleaning up temporary files...\n";
        script << "set /a cleanup_retries=0\n";
        script << ":cleanup_loop\n";
        script << "rd /s /q \"" << staging_path_str << "\" >nul 2>&1\n";
        script << "if exist \"" << staging_path_str << "\" (\n";
        script << "    set /a cleanup_retries+=1\n";
        script << "    if !cleanup_retries! lss 5 (\n";
        script << "        timeout /t 1 /nobreak >nul\n";
        script << "        goto cleanup_loop\n";
        script << "    )\n";
        script << ")\n\n";

        // Self-delete script
        script << "del \"%~f0\" >nul 2>&1\n";
        script << "exit /b 0\n";

        script.close();
        return true;
    } catch (...) {
        return false;
    }
}

bool UpdaterService::LaunchUpdateHelper() {
    std::filesystem::path sc = app_directory / "update_staging" / "apply_update.bat";
    if (!std::filesystem::exists(sc)) return false;

    // We launch via cmd /c and detach so the script survives Citron's death
    return QProcess::startDetached("cmd.exe", QStringList() << "/C" << QString::fromStdString(sc.string()));
}
#endif

void UpdaterService::OnDownloadProgress(qint64 r, qint64 t) {
    if (t > 0) emit UpdateDownloadProgress(static_cast<int>((r * 100) / t), r, t);
}

bool UpdaterService::EnsureDirectoryExists(const std::filesystem::path& p) const {
    std::error_code ec;
    return std::filesystem::create_directories(p, ec) || std::filesystem::exists(p);
}

bool UpdaterService::CleanupFiles() {
    std::error_code ec;
    if (std::filesystem::exists(temp_download_path)) {
        std::filesystem::remove_all(temp_download_path, ec);
    }
    return !ec;
}

} // namespace Updater
