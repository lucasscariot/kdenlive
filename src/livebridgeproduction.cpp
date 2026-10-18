/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "core.h"
#include "dialogs/wizard.h"
#include "kdenlivesettings.h"
#include "livebridge.h"
#include "mainwindow.h"
#include "project/projectmanager.h"
#include "render/renderserver.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <mlt++/MltProfile.h>
#ifdef Q_OS_UNIX
#include <csignal>
#endif

namespace {
QString journalPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/mcp-renders.json");
}

QJsonObject error(const QString &code, const QString &message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

bool run(QProcess &process, const QString &program, const QStringList &arguments, int timeout)
{
    process.start(program, arguments);
    if (!process.waitForStarted(3000)) return false;
    if (!process.waitForFinished(timeout)) {
        process.kill();
        process.waitForFinished(3000);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool integer(const QJsonObject &object, const QString &key, int minimum = 0)
{
    const auto value = object.value(key);
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= minimum && number <= std::numeric_limits<int>::max() && std::floor(number) == number;
}

bool keys(const QJsonObject &object, const QStringList &allowed)
{
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key())) return false;
    return true;
}

/** Children of a process, from /proc; kdenlive_render runs melt as a child that must not outlive it. */
QList<qint64> childProcesses(qint64 pid)
{
    QList<qint64> result;
#ifdef Q_OS_LINUX
    const QDir proc(QStringLiteral("/proc"));
    for (const auto &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const qint64 child = entry.toLongLong(&numeric);
        if (!numeric) continue;
        QFile stat(QStringLiteral("/proc/%1/stat").arg(entry));
        if (!stat.open(QIODevice::ReadOnly)) continue;
        const QByteArray line = stat.readAll();
        // The command name is in parentheses and may contain spaces; the parent pid is the second field after it.
        const auto fields = line.mid(line.lastIndexOf(')') + 2).split(' ');
        if (fields.size() > 1 && fields.at(1).toLongLong() == pid) result.append(child);
    }
#else
    Q_UNUSED(pid)
#endif
    return result;
}

void terminateProcessTree(qint64 pid)
{
#ifdef Q_OS_UNIX
    if (pid <= 0) return;
    for (qint64 child : childProcesses(pid))
        terminateProcessTree(child);
    ::kill(pid_t(pid), SIGTERM);
#else
    Q_UNUSED(pid)
#endif
}
} // namespace

void LiveBridge::loadRenderJournal()
{
    QFile file(journalPath());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return;
    const auto document = QJsonDocument::fromJson(file.readAll());
    for (const auto &value : document.object().value(QStringLiteral("jobs")).toArray()) {
        const auto job = value.toObject();
        const QString path = job.value(QStringLiteral("path")).toString();
        if (!QFileInfo(path).isAbsolute() || m_renders.size() >= 100) continue;
        RenderState state;
        state.preset = job.value(QStringLiteral("preset")).toString();
        state.status = job.value(QStringLiteral("status")).toString();
        // A missing editor connection is not proof that its detached renderer stopped or finished.
        if (state.status == QLatin1String("running") || state.status == QLatin1String("starting")) state.status = QStringLiteral("unconfirmed");
        state.progress = job.value(QStringLiteral("progress")).toInt();
        state.frame = job.value(QStringLiteral("frame")).toInt();
        state.error = job.value(QStringLiteral("error")).toString();
        state.started = job.value(QStringLiteral("started")).toInteger();
        state.pid = job.value(QStringLiteral("pid")).toInteger();
        state.playlist = job.value(QStringLiteral("playlist")).toString();
        state.expectedFrames = job.value(QStringLiteral("expectedFrames")).toInt();
        state.start = job.value(QStringLiteral("start")).toInt();
        state.width = job.value(QStringLiteral("width")).toInt();
        state.height = job.value(QStringLiteral("height")).toInt();
        state.fps = job.value(QStringLiteral("fps")).toDouble();
        state.preview = job.value(QStringLiteral("preview")).toBool();
        m_renders.insert(path, state);
    }
}

bool LiveBridge::saveRenderJournal()
{
    QJsonArray jobs;
    auto paths = m_renders.keys();
    std::sort(paths.begin(), paths.end(), [this](const QString &a, const QString &b) { return m_renders.value(a).started > m_renders.value(b).started; });
    for (const auto &path : paths.mid(0, 100)) {
        const auto it = m_renders.constFind(path);
        jobs.append(QJsonObject{{"path", it.key()},
                                {"preset", it->preset},
                                {"status", it->status},
                                {"progress", it->progress},
                                {"frame", it->frame},
                                {"error", it->error},
                                {"started", it->started},
                                {"pid", it->pid},
                                {"playlist", it->playlist},
                                {"expectedFrames", it->expectedFrames},
                                {"start", it->start},
                                {"width", it->width},
                                {"height", it->height},
                                {"fps", it->fps},
                                {"preview", it->preview}});
    }
    const QString path = journalPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(QJsonObject{{"version", 1}, {"jobs", jobs}}).toJson(QJsonDocument::Compact));
    if (!file.commit()) return false;
    m_renderJournalSaved = QDateTime::currentSecsSinceEpoch();
    return true;
}

QJsonObject LiveBridge::renderVerify(const QJsonObject &arguments)
{
    for (auto it = arguments.begin(); it != arguments.end(); ++it)
        if (it.key() != QLatin1String("path") && it.key() != QLatin1String("includeAudio"))
            return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected path and optional includeAudio."));
    if (!arguments.value(QStringLiteral("path")).isString() ||
        (arguments.contains(QStringLiteral("includeAudio")) && !arguments.value(QStringLiteral("includeAudio")).isBool()))
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected path and boolean includeAudio."));
    const QString path = arguments.value(QStringLiteral("path")).toString();
    auto job = m_renders.find(path);
    if (job == m_renders.end()) return error(QStringLiteral("UNKNOWN_JOB"), QStringLiteral("Only exports started through MCP can be verified."));
    if (job->status == QLatin1String("starting") || job->status == QLatin1String("running"))
        return error(QStringLiteral("RENDER_BUSY"), QStringLiteral("Wait for the renderer to finish."));
#ifdef Q_OS_LINUX
    QFile commandLine(QStringLiteral("/proc/%1/cmdline").arg(job->pid));
    if (job->status == QLatin1String("unconfirmed") && job->pid > 0 && commandLine.open(QIODevice::ReadOnly) &&
        commandLine.readAll().split('\0').contains(job->playlist.toUtf8()))
        return error(QStringLiteral("RENDER_BUSY"), QStringLiteral("The detached renderer is still running after the editor restarted."));
    if (job->status == QLatin1String("unconfirmed")) {
        job->status = QStringLiteral("interrupted");
        saveRenderJournal();
    }
#endif
    QProcess probe;
    if (!run(probe, KdenliveSettings::ffprobepath(),
             {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_streams"), QStringLiteral("-show_format"), QStringLiteral("-of"),
              QStringLiteral("json"), path},
             15000))
        return error(QStringLiteral("VERIFICATION_FAILED"),
                     QStringLiteral("Export cannot be probed: %1").arg(QString::fromUtf8(probe.readAllStandardError()).left(4096)));
    const auto metadata = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
    QJsonObject video;
    bool audio = false;
    for (const auto &value : metadata.value(QStringLiteral("streams")).toArray()) {
        const auto stream = value.toObject();
        if (stream.value(QStringLiteral("codec_type")) == QLatin1String("video") && video.isEmpty()) video = stream;
        if (stream.value(QStringLiteral("codec_type")) == QLatin1String("audio")) audio = true;
    }
    const double duration = metadata.value(QStringLiteral("format")).toObject().value(QStringLiteral("duration")).toString().toDouble();
    const auto rate = video.value(QStringLiteral("avg_frame_rate")).toString().split(QLatin1Char('/'));
    const double videoFps = rate.size() == 2 && rate[1].toDouble() > 0 ? rate[0].toDouble() / rate[1].toDouble() : 0;
    if (job->fps <= 0 || job->expectedFrames <= 0 || video.isEmpty() || video.value(QStringLiteral("width")).toInt() != job->width ||
        video.value(QStringLiteral("height")).toInt() != job->height || !std::isfinite(duration) || std::abs(videoFps - job->fps) > .001 ||
        std::abs(duration - job->expectedFrames / job->fps) > qMax(.1, 2 / job->fps))
        return error(QStringLiteral("VERIFICATION_FAILED"), QStringLiteral("Export dimensions or duration do not match the render request."));
    QProcess decoder;
    if (!run(decoder, KdenliveSettings::ffmpegpath(),
             {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-i"), path, QStringLiteral("-progress"),
              QStringLiteral("pipe:1"), QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")},
             60000))
        return error(QStringLiteral("VERIFICATION_FAILED"),
                     QStringLiteral("Full decoding failed or exceeded 60 seconds: %1").arg(QString::fromUtf8(decoder.readAllStandardError()).left(4096)));
    const auto decodeErrors = decoder.readAllStandardError();
    if (!decodeErrors.trimmed().isEmpty())
        return error(QStringLiteral("VERIFICATION_FAILED"), QStringLiteral("Decode errors: %1").arg(QString::fromUtf8(decodeErrors).left(4096)));
    int decodedFrames = 0;
    auto frames = QRegularExpression(QStringLiteral("(?:^|\n)frame=([0-9]+)")).globalMatch(QString::fromUtf8(decoder.readAllStandardOutput()));
    while (frames.hasNext())
        decodedFrames = frames.next().captured(1).toInt();
    if (decodedFrames != job->expectedFrames)
        return error(QStringLiteral("VERIFICATION_FAILED"), QStringLiteral("Decoded %1 frames; expected %2.").arg(decodedFrames).arg(job->expectedFrames));
    QJsonObject result{
        {"ok", true},          {"path", path},          {"verified", true}, {"durationSeconds", duration}, {"expectedFrames", job->expectedFrames},
        {"width", job->width}, {"height", job->height}, {"fps", job->fps},  {"hasAudio", audio},           {"fullDecode", true}};
    if (arguments.value(QStringLiteral("includeAudio")).toBool()) {
        if (!job->preview || duration > 10.1)
            return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("includeAudio is limited to short MCP preview renders."));
        if (!audio) return error(QStringLiteral("NO_AUDIO"), QStringLiteral("The preview has no audio stream."));
        QTemporaryFile wav(QDir::tempPath() + QStringLiteral("/kdenlive-mcp-audio-XXXXXX.wav"));
        if (!wav.open()) return error(QStringLiteral("PREVIEW_FAILED"), QStringLiteral("Cannot create temporary preview audio."));
        const QString wavPath = wav.fileName();
        wav.close();
        QProcess extract;
        if (!run(extract, KdenliveSettings::ffmpegpath(),
                 {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-i"), path,
                  QStringLiteral("-vn"), QStringLiteral("-t"), QStringLiteral("10"), QStringLiteral("-ar"), QStringLiteral("16000"), QStringLiteral("-ac"),
                  QStringLiteral("1"), QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), wavPath},
                 15000))
            return error(QStringLiteral("PREVIEW_FAILED"), QStringLiteral("Cannot extract preview audio."));
        QFile sound(wavPath);
        if (!sound.open(QIODevice::ReadOnly) || sound.size() > 400000)
            return error(QStringLiteral("PREVIEW_FAILED"), QStringLiteral("Preview audio is missing or too large."));
        result.insert(QStringLiteral("audio"),
                      QJsonObject{{"data", QString::fromLatin1(sound.readAll().toBase64())}, {"mimeType", QStringLiteral("audio/wav")}});
    }
    job->status = QStringLiteral("finished");
    job->progress = 100;
    saveRenderJournal();
    result.insert(QStringLiteral("resource"),
                  QJsonObject{{"uri", QUrl::fromLocalFile(path).toString()},
                              {"name", QFileInfo(path).fileName()},
                              {"mimeType", job->preview ? QStringLiteral("video/mp4") : QStringLiteral("application/octet-stream")}});
    return result;
}

QJsonObject LiveBridge::renderCancel(const QJsonObject &arguments)
{
    if (!keys(arguments, {QStringLiteral("path")}) || !arguments.value(QStringLiteral("path")).isString())
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected the path of a render started through MCP."));
    const QString path = arguments.value(QStringLiteral("path")).toString();
    renderStatus();
    auto job = m_renders.find(path);
    if (job == m_renders.end()) return error(QStringLiteral("UNKNOWN_JOB"), QStringLiteral("Only renders started through MCP can be cancelled."));
    const QString before = job->status;
    if (before != QLatin1String("running") && before != QLatin1String("starting") && before != QLatin1String("unconfirmed"))
        return error(QStringLiteral("NOT_RUNNING"), QStringLiteral("The render is %1, nothing to cancel.").arg(before));
    // The renderer listens on the render server's socket once it reports progress, and then removes its own partial output cleanly.
    // A job that has only just started gets a moment to connect before the process tree is stopped by force.
    QElapsedTimer timer;
    timer.start();
    while (job->status == QLatin1String("starting") && timer.elapsed() < 4000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    bool signalled = false;
    if (job->status == QLatin1String("running")) {
        if (auto *server = pCore->window()->findChild<RenderServer *>()) {
            server->abortJob(path);
            signalled = true;
        }
    }
    timer.restart();
    while (signalled && timer.elapsed() < 3000 && job->status == QLatin1String("running"))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (job->status != QLatin1String("aborted")) {
        // Not connected yet, or no answer: stop melt and the renderer wrapper directly.
        terminateProcessTree(job->pid);
        job->status = QStringLiteral("aborted");
        job->error = QStringLiteral("Cancelled through MCP.");
    }
    bool outputRemoved = false;
    if (QFile::exists(path)) {
        QElapsedTimer release;
        release.start();
        // Give the renderer a moment to close the file before removing the partial output.
        while (release.elapsed() < 2000 && QFile::exists(path) && !(outputRemoved = QFile::remove(path)))
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    saveRenderJournal();
    return {{"ok", true}, {"path", path}, {"status", job->status}, {"signalled", signalled}, {"outputRemoved", outputRemoved}};
}

QJsonObject LiveBridge::frameCapture(const QJsonObject &arguments)
{
    if (!bind()) return error(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    if (!keys(arguments, {QStringLiteral("position"), QStringLiteral("width"), QStringLiteral("path")}) || !integer(arguments, QStringLiteral("position")) ||
        (arguments.contains(QStringLiteral("width")) && !integer(arguments, QStringLiteral("width"), 64)) ||
        (arguments.contains(QStringLiteral("path")) && !arguments.value(QStringLiteral("path")).isString()))
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Expected position, an optional width of at least 64 and an optional .png path."));
    const int position = arguments.value(QStringLiteral("position")).toInt();
    if (position >= pCore->projectDuration()) return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Position is past the end of the timeline."));
    auto &profile = pCore->getProjectProfile();
    int width = std::min(arguments.value(QStringLiteral("width")).toInt(540), profile.width());
    width -= width % 2;
    int height = qRound(width * double(profile.height()) / profile.width());
    height += height % 2;
    const QString target = arguments.value(QStringLiteral("path")).toString();
    if (!target.isEmpty()) {
        const QFileInfo output(target);
        if (!output.isAbsolute() || output.exists() || output.suffix().compare(QLatin1String("png"), Qt::CaseInsensitive) != 0 ||
            !QFileInfo(output.absolutePath()).isDir() || !QFileInfo(output.absolutePath()).isWritable())
            return error(QStringLiteral("INVALID_PATH"), QStringLiteral("path must be a new absolute .png file in a writable folder."));
    }
    Wizard::fixKdenliveRenderPath();
    const QString melt = KdenliveSettings::meltpath();
    if (!QFile::exists(melt)) return error(QStringLiteral("RENDERER_MISSING"), QStringLiteral("melt is not configured."));
    // A separate melt process renders the frame: decoding the sequence a second time inside the editor competed with the monitor for MLT's
    // producer cache and could abort the application.
    QTemporaryDir folder(QDir::tempPath() + QStringLiteral("/kdenlive-mcp-capture-XXXXXX"));
    if (!folder.isValid()) return error(QStringLiteral("CAPTURE_FAILED"), QStringLiteral("Cannot create a temporary folder."));
    const QString scene = pCore->projectManager()->projectSceneList(QString(), true).first;
    const QString playlist = folder.filePath(QStringLiteral("scene.mlt"));
    QFile sceneFile(playlist);
    if (scene.isEmpty() || !sceneFile.open(QIODevice::WriteOnly) || sceneFile.write(scene.toUtf8()) != scene.toUtf8().size())
        return error(QStringLiteral("CAPTURE_FAILED"), QStringLiteral("Cannot write the sequence for capture."));
    sceneFile.close();
    const QString output = target.isEmpty() ? folder.filePath(QStringLiteral("frame.png")) : target;
    QStringList args;
    const QString profilePath = pCore->getCurrentProfilePath();
    if (!profilePath.isEmpty()) args << QStringLiteral("-profile") << profilePath;
    args << playlist << QStringLiteral("in=%1").arg(position) << QStringLiteral("out=%1").arg(position) << QStringLiteral("-consumer")
         << QStringLiteral("avformat:") + output << QStringLiteral("vcodec=png") << QStringLiteral("pix_fmt=rgb24") << QStringLiteral("width=%1").arg(width)
         << QStringLiteral("height=%1").arg(height) << QStringLiteral("real_time=0") << QStringLiteral("audio_off=1") << QStringLiteral("terminate_on_pause=1");
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    if (!run(process, melt, args, 90000) || !QFile::exists(output))
        return error(QStringLiteral("CAPTURE_FAILED"),
                     QStringLiteral("melt could not render the frame: %1").arg(QString::fromUtf8(process.readAll()).right(2048).trimmed()));
    const QImage image(output);
    if (image.isNull()) return error(QStringLiteral("CAPTURE_FAILED"), QStringLiteral("The rendered frame cannot be read."));
    QJsonObject result{{"ok", true}, {"position", position}, {"width", image.width()}, {"height", image.height()}};
    if (!target.isEmpty()) {
        result.insert(QStringLiteral("path"), output);
        result.insert(QStringLiteral("bytes"), QFileInfo(output).size());
        result.insert(QStringLiteral("resource"),
                      QJsonObject{{"uri", QUrl::fromLocalFile(output).toString()}, {"name", QFileInfo(output).fileName()}, {"mimeType", "image/png"}});
        return result;
    }
    QFile png(output);
    if (!png.open(QIODevice::ReadOnly)) return error(QStringLiteral("CAPTURE_FAILED"), QStringLiteral("Cannot read the rendered frame."));
    result.insert(QStringLiteral("image"), QJsonObject{{"data", QString::fromLatin1(png.readAll().toBase64())}, {"mimeType", "image/png"}});
    return result;
}
