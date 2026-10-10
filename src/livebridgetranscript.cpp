/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
// Transcripts, silence detection and transcript-addressed cuts for the live editing bridge; see docs/native-mcp.md "Transcripts and silence".
#include "livebridge.h"

#include "bin/bin.h"
#include "bin/model/markerlistmodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "macros.hpp"
#include "pythoninterfaces/speechtotextvosk.h"
#include "pythoninterfaces/speechtotextwhisper.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <KLocalizedString>

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <limits>

namespace {
QJsonObject error(const QString &code, const QString &message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

bool keys(const QJsonObject &object, const QStringList &allowed)
{
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key())) return false;
    return true;
}

bool integer(const QJsonObject &object, const QString &key, int minimum = 0)
{
    const auto value = object.value(key);
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= minimum && number <= std::numeric_limits<int>::max() && std::floor(number) == number;
}

/** A finite number in [minimum, maximum] under key, or fallback when the key is absent. */
bool number(const QJsonObject &object, const QString &key, double minimum, double maximum, double &value)
{
    if (!object.contains(key)) return true;
    const auto item = object.value(key);
    if (!item.isDouble() || !std::isfinite(item.toDouble()) || item.toDouble() < minimum || item.toDouble() > maximum) return false;
    value = item.toDouble();
    return true;
}

bool optionalBool(const QJsonObject &object, const QString &key, bool &value)
{
    if (!object.contains(key)) return true;
    if (!object.value(key).isBool()) return false;
    value = object.value(key).toBool();
    return true;
}

QJsonArray idArray(const QList<int> &ids)
{
    QJsonArray result;
    for (int id : ids)
        result.append(id);
    return result;
}

QList<int> sortedIds(const QSet<int> &ids)
{
    QList<int> result(ids.cbegin(), ids.cend());
    std::sort(result.begin(), result.end());
    return result;
}

template <typename Step> bool runStep(Fun &undo, Fun &redo, Step &&step)
{
    Fun localUndo = [] { return true; };
    Fun localRedo = [] { return true; };
    if (!step(localUndo, localRedo)) return false;
    UPDATE_UNDO_REDO_NOLOCK(localRedo, localUndo, undo, redo);
    return true;
}

const QString transcriptProperty = QStringLiteral("kdenlive:mcp_transcript");
const QString speechProperty = QStringLiteral("kdenlive:speech");
// Longest span silence detection decodes in one call (two hours of audio).
constexpr double maxAnalysisSeconds = 7200;
constexpr int maxWords = 100000;
constexpr int maxCutRanges = 1000;

double millis(double seconds)
{
    return std::round(seconds * 1000.0) / 1000.0;
}

int frameOf(double seconds, double fps)
{
    return int(std::llround(seconds * fps));
}

struct Word
{
    double start{0};
    double end{0};
    QString text;
    double confidence{-1};
    int segment{-1};
    bool interpolated{false};
};

struct Transcript
{
    QString language;
    QString engine;
    QString format;
    // "mcp" for kdenlive:mcp_transcript, "kdenlive:speech" for a transcript made in the Text-based edit panel.
    QString origin;
    QList<Word> words;
};

QJsonObject invalidTranscript(const QString &message)
{
    return error(QStringLiteral("INVALID_TRANSCRIPT"), message);
}

/** SRT (HH:MM:SS,mmm) and WebVTT (HH:MM:SS.mmm or MM:SS.mmm) cue times. */
bool cueTime(const QString &text, double &seconds)
{
    static const QRegularExpression pattern(QStringLiteral("^(?:(\\d+):)?(\\d{1,2}):(\\d{2})[,.](\\d{1,3})$"));
    const auto match = pattern.match(text.trimmed());
    if (!match.hasMatch()) return false;
    const QString fraction = match.captured(4);
    seconds = match.captured(1).toDouble() * 3600 + match.captured(2).toDouble() * 60 + match.captured(3).toDouble() +
              fraction.toDouble() / std::pow(10.0, fraction.size());
    return true;
}

/** Splits a timed phrase into words, dividing its duration equally between them. */
void interpolate(QList<Word> &words, double start, double end, const QString &text, int segment)
{
    static const QRegularExpression tags(QStringLiteral("<[^>]*>|\\{\\\\[^}]*\\}"));
    const QStringList tokens = QString(text).remove(tags).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const double step = tokens.isEmpty() ? 0 : (end - start) / tokens.size();
    for (int i = 0; i < tokens.size(); ++i) {
        Word word;
        word.start = millis(start + step * i);
        word.end = i == tokens.size() - 1 ? millis(end) : millis(start + step * (i + 1));
        word.text = tokens.at(i);
        word.segment = segment;
        word.interpolated = true;
        words.append(word);
    }
}

QJsonObject parseCues(const QString &text, QList<Word> &words)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (normalized.startsWith(QChar(0xFEFF))) normalized.remove(0, 1);
    static const QRegularExpression blank(QStringLiteral("\n[ \t]*\n"));
    int segment = 0;
    int cues = 0;
    for (const auto &block : normalized.split(blank, Qt::SkipEmptyParts)) {
        QStringList lines;
        for (const auto &line : block.split(QLatin1Char('\n')))
            if (!line.trimmed().isEmpty()) lines << line.trimmed();
        if (lines.isEmpty()) continue;
        int timing = -1;
        for (int i = 0; i < lines.size() && timing < 0; ++i)
            if (lines.at(i).contains(QStringLiteral("-->"))) timing = i;
        if (timing < 0) {
            // WebVTT header, NOTE, STYLE and REGION blocks carry no cue.
            if (cues == 0 && lines.constFirst().startsWith(QStringLiteral("WEBVTT"))) continue;
            if (lines.constFirst().startsWith(QStringLiteral("NOTE")) || lines.constFirst() == QLatin1String("STYLE") ||
                lines.constFirst() == QLatin1String("REGION"))
                continue;
            return invalidTranscript(QStringLiteral("Cue %1 has no 'start --> end' timing line.").arg(cues + 1));
        }
        const QString line = lines.at(timing);
        double start = 0, end = 0;
        // WebVTT cue settings may follow the end time.
        if (!cueTime(line.section(QStringLiteral("-->"), 0, 0), start) ||
            !cueTime(line.section(QStringLiteral("-->"), 1).trimmed().section(QLatin1Char(' '), 0, 0), end) || end < start)
            return invalidTranscript(QStringLiteral("Cue %1 has an invalid timing line: %2").arg(cues + 1).arg(line));
        ++cues;
        const QString content = lines.mid(timing + 1).join(QLatin1Char(' '));
        const qsizetype before = words.size();
        interpolate(words, start, end, content, segment);
        if (words.size() > before) ++segment;
    }
    if (cues == 0) return invalidTranscript(QStringLiteral("No subtitle cues found."));
    return {};
}

QJsonObject wordFromJson(const QJsonValue &value, int index, Word &word)
{
    const auto object = value.toObject();
    const auto start = object.value(QStringLiteral("start"));
    const auto end = object.value(QStringLiteral("end"));
    QJsonValue text = object.value(QStringLiteral("text"));
    if (text.isUndefined()) text = object.value(QStringLiteral("word"));
    if (!value.isObject() || !start.isDouble() || !end.isDouble() || !text.isString() || text.toString().trimmed().isEmpty())
        return invalidTranscript(QStringLiteral("Word %1 needs numeric start and end (seconds) and a non-empty text.").arg(index));
    word.start = millis(start.toDouble());
    word.end = millis(end.toDouble());
    word.text = text.toString().simplified();
    QJsonValue confidence = object.value(QStringLiteral("confidence"));
    if (confidence.isUndefined()) confidence = object.value(QStringLiteral("probability"));
    if (!confidence.isUndefined() && !confidence.isNull()) {
        if (!confidence.isDouble() || confidence.toDouble() < 0 || confidence.toDouble() > 1)
            return invalidTranscript(QStringLiteral("Word %1 confidence must be between 0 and 1.").arg(index));
        word.confidence = confidence.toDouble();
    }
    const auto segment = object.value(QStringLiteral("segment"));
    if (!segment.isUndefined() && !segment.isNull()) {
        if (!integer(object, QStringLiteral("segment"))) return invalidTranscript(QStringLiteral("Word %1 segment must be an index.").arg(index));
        word.segment = segment.toInt();
    }
    word.interpolated = object.value(QStringLiteral("interpolated")).toBool(false);
    return {};
}

/** Parses import text in one of the transcriptFormats into words sorted by start time. */
QJsonObject parseTranscript(const QString &format, const QString &text, Transcript &transcript)
{
    QList<Word> words;
    if (format == QLatin1String("srt") || format == QLatin1String("vtt")) {
        if (auto rejected = parseCues(text, words); !rejected.isEmpty()) return rejected;
    } else {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError) return invalidTranscript(QStringLiteral("Not valid JSON: %1.").arg(parseError.errorString()));
        const auto object = document.object();
        transcript.language = object.value(QStringLiteral("language")).toString();
        if (format == QLatin1String("whisper")) {
            // openai-whisper's JSON output: segments[] with optional words[] (word_timestamps=True).
            if (!object.value(QStringLiteral("segments")).isArray())
                return invalidTranscript(QStringLiteral("Whisper JSON needs a segments array (openai-whisper --output_format json)."));
            const auto segments = object.value(QStringLiteral("segments")).toArray();
            for (int i = 0; i < segments.size(); ++i) {
                const auto segment = segments.at(i).toObject();
                const auto list = segment.value(QStringLiteral("words")).toArray();
                if (list.isEmpty()) {
                    const auto start = segment.value(QStringLiteral("start"));
                    const auto end = segment.value(QStringLiteral("end"));
                    if (!start.isDouble() || !end.isDouble() || end.toDouble() < start.toDouble())
                        return invalidTranscript(QStringLiteral("Segment %1 needs numeric start and end.").arg(i));
                    interpolate(words, start.toDouble(), end.toDouble(), segment.value(QStringLiteral("text")).toString(), i);
                    continue;
                }
                for (int j = 0; j < list.size(); ++j) {
                    Word word;
                    if (auto rejected = wordFromJson(list.at(j), int(words.size()), word); !rejected.isEmpty()) return rejected;
                    word.segment = i;
                    words.append(word);
                }
            }
        } else {
            // Our own shape: {language?, engine?, words: [...]} or a bare words array; extra keys (desktop_transcript output) are ignored.
            const auto list = document.isArray() ? document.array() : object.value(QStringLiteral("words")).toArray();
            if (!document.isArray() && !object.value(QStringLiteral("words")).isArray())
                return invalidTranscript(QStringLiteral("JSON transcripts are a words array or an object with words[]."));
            transcript.engine = object.value(QStringLiteral("engine")).toString();
            for (int i = 0; i < list.size(); ++i) {
                Word word;
                if (auto rejected = wordFromJson(list.at(i), i, word); !rejected.isEmpty()) return rejected;
                words.append(word);
            }
        }
    }
    if (words.size() > maxWords) return invalidTranscript(QStringLiteral("At most %1 words per clip.").arg(maxWords));
    for (int i = 0; i < words.size(); ++i) {
        const auto &word = words.at(i);
        if (!std::isfinite(word.start) || !std::isfinite(word.end) || word.start < 0 || word.end < word.start)
            return invalidTranscript(QStringLiteral("Word %1 (%2) needs 0 <= start <= end.").arg(i).arg(word.text));
        if (word.text.size() > 1000) return invalidTranscript(QStringLiteral("Word %1 is longer than 1000 characters.").arg(i));
    }
    std::stable_sort(words.begin(), words.end(), [](const Word &a, const Word &b) { return a.start < b.start; });
    transcript.words = words;
    transcript.format = format;
    return {};
}

QJsonObject storedWord(const Word &word)
{
    QJsonObject result{{"start", word.start}, {"end", word.end}, {"text", word.text}};
    if (word.confidence >= 0) result.insert(QStringLiteral("confidence"), word.confidence);
    if (word.segment >= 0) result.insert(QStringLiteral("segment"), word.segment);
    if (word.interpolated) result.insert(QStringLiteral("interpolated"), true);
    return result;
}

QString speechDigest(const QString &html)
{
    static const QRegularExpression space(QStringLiteral("\\s"));
    return QString::fromLatin1(QCryptographicHash::hash(QString(html).remove(space).toUtf8(), QCryptographicHash::Sha1).toHex());
}

bool endsSentence(const QString &text)
{
    static const QRegularExpression end(QStringLiteral("[.?!\\x{2026}\\x{3002}\\x{FF1F}\\x{FF01}][\"'\\x{201D}\\x{2019})\\]]*$"));
    return end.match(text).hasMatch();
}

struct Segment
{
    int first;
    int last;
};

/** Segments of words[first..last]: the stored ones when every word carries a segment index, else split after sentence punctuation and pauses. */
QList<Segment> segmentsOf(const QList<Word> &words, int first, int last, double minPause)
{
    QList<Segment> result;
    if (first > last) return result;
    bool stored = true;
    for (int i = first; i <= last && stored; ++i)
        stored = words.at(i).segment >= 0;
    int start = first;
    for (int i = first; i < last; ++i) {
        const auto &word = words.at(i);
        const auto &next = words.at(i + 1);
        const bool boundary = stored ? next.segment != word.segment : endsSentence(word.text) || next.start - word.end > minPause;
        if (boundary) {
            result.append({start, i});
            start = i + 1;
        }
    }
    result.append({start, last});
    return result;
}

/** Text-based edit panel HTML (kdenlive:speech): one block per segment, one anchor "binId#start:end" per word, "No speech" blocks between. */
QString speechHtml(const QString &binId, const QList<Word> &words, double fps)
{
    QTextDocument document;
    QTextCursor cursor(&document);
    const QTextCharFormat plain;
    const auto anchor = [&binId](double start, double end) {
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorHref(QStringLiteral("%1#%2:%3").arg(binId, QString::number(start, 'f', 3), QString::number(end, 'f', 3)));
        return format;
    };
    double last = 0;
    bool first = true;
    for (const auto &segment : segmentsOf(words, 0, int(words.size()) - 1, 0.5)) {
        const double start = words.at(segment.first).start;
        if (start - last > 1.0 / fps) {
            if (!first) cursor.insertBlock();
            cursor.insertText(i18n("No speech"), anchor(last, start - 1.0 / fps));
            first = false;
        }
        if (!first) cursor.insertBlock();
        first = false;
        for (int i = segment.first; i <= segment.last; ++i) {
            cursor.insertText(words.at(i).text, anchor(words.at(i).start, words.at(i).end));
            cursor.insertText(QStringLiteral(" "), plain);
        }
        last = words.at(segment.last).end;
    }
    return document.toHtml();
}

QList<Word> parseSpeechHtml(const QString &html)
{
    QList<Word> words;
    QTextDocument document;
    document.setHtml(html);
    static const QRegularExpression href(QStringLiteral("#([-+0-9.eE]+):([-+0-9.eE]+)$"));
    const QString noSpeech = i18n("No speech");
    int segment = 0;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        bool any = false;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isAnchor()) continue;
            const auto match = href.match(fragment.charFormat().anchorHref());
            const QString text = fragment.text().simplified();
            if (!match.hasMatch() || text.isEmpty() || text == noSpeech) continue;
            Word word;
            word.start = millis(match.captured(1).toDouble());
            word.end = millis(qMax(match.captured(1).toDouble(), match.captured(2).toDouble()));
            word.text = text;
            word.segment = segment;
            words.append(word);
            any = true;
        }
        if (any) ++segment;
    }
    std::stable_sort(words.begin(), words.end(), [](const Word &a, const Word &b) { return a.start < b.start; });
    return words;
}

/** The clip's transcript: kdenlive:mcp_transcript unless the Text-based edit panel has written a newer kdenlive:speech since. */
bool loadTranscript(const std::shared_ptr<ProjectClip> &clip, Transcript &transcript)
{
    const QString stored = clip->getProducerProperty(transcriptProperty);
    const QString speech = clip->getProducerProperty(speechProperty);
    if (!stored.isEmpty()) {
        const auto object = QJsonDocument::fromJson(stored.toUtf8()).object();
        if (speech.isEmpty() || object.value(QStringLiteral("speechDigest")).toString() == speechDigest(speech)) {
            transcript.language = object.value(QStringLiteral("language")).toString();
            transcript.engine = object.value(QStringLiteral("engine")).toString();
            transcript.format = object.value(QStringLiteral("format")).toString();
            transcript.origin = QStringLiteral("mcp");
            const auto list = object.value(QStringLiteral("words")).toArray();
            for (int i = 0; i < list.size(); ++i) {
                Word word;
                if (wordFromJson(list.at(i), i, word).isEmpty()) transcript.words.append(word);
            }
            return true;
        }
    }
    if (speech.isEmpty()) return false;
    transcript.words = parseSpeechHtml(speech);
    transcript.engine = QStringLiteral("kdenlive");
    transcript.format = QStringLiteral("speech");
    transcript.origin = speechProperty;
    return !transcript.words.isEmpty();
}

/** Stores a transcript (and the matching Text-based edit HTML) as one Undo entry through the bin's Edit Clip command. */
QJsonObject storeTranscript(KdenliveDoc *document, const std::shared_ptr<ProjectClip> &clip, const Transcript &transcript, const QString &undoText)
{
    const double fps = pCore->getCurrentFps();
    QJsonArray list;
    for (const auto &word : transcript.words)
        list.append(storedWord(word));
    const QString html = transcript.words.isEmpty() ? QString() : speechHtml(clip->binId(), transcript.words, fps);
    const QJsonObject stored{{"version", 1},
                             {"language", transcript.language},
                             {"engine", transcript.engine},
                             {"format", transcript.format},
                             {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                             {"speechDigest", speechDigest(html)},
                             {"words", list}};
    const QMap<QString, QString> before{{transcriptProperty, clip->getProducerProperty(transcriptProperty)},
                                        {speechProperty, clip->getProducerProperty(speechProperty)}};
    const QMap<QString, QString> after{{transcriptProperty, QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact))}, {speechProperty, html}};
    auto stack = document->commandStack();
    // The macro keeps Kdenlive from merging two quick Edit Clip commands on the same clip into one Undo entry.
    stack->beginMacro(undoText);
    pCore->bin()->slotEditClipCommand(clip->binId(), before, after);
    stack->endMacro();
    if (clip->getProducerProperty(transcriptProperty) != after.value(transcriptProperty))
        return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("The bin rejected the transcript."));
    return {};
}

/** One timeline clip as a window onto its bin clip's source: sourceIn counts frames of the speed-adjusted source. */
struct Instance
{
    int clipId{-1};
    int trackId{-1};
    QString binId;
    int position{0};
    int duration{0};
    int sourceIn{0};
    double speed{1};
    double windowStart(double fps) const { return sourceIn * speed / fps; }
    double windowEnd(double fps) const { return (qint64(sourceIn) + duration) * speed / fps; }
    /** Timeline frame of a source time; time-remap curves are ignored. */
    int timeline(double seconds, double fps) const
    {
        const qint64 frame = position + std::llround(seconds * fps / speed) - sourceIn;
        return int(qBound<qint64>(position, frame, qint64(position) + duration));
    }
    /** Source seconds of a timeline frame inside the clip. */
    double source(int frame, double fps) const { return (sourceIn + double(frame - position)) * speed / fps; }
};

Instance instanceOf(TimelineItemModel *timeline, int clipId)
{
    Instance result;
    result.clipId = clipId;
    result.trackId = timeline->getClipTrackId(clipId);
    result.binId = timeline->getClipBinId(clipId);
    result.position = timeline->getClipPosition(clipId);
    result.duration = timeline->getClipPlaytime(clipId);
    result.sourceIn = timeline->getClipInOut(clipId).first;
    result.speed = timeline->getClipSpeed(clipId);
    return result;
}

QList<int> allTracks(TimelineItemModel *timeline)
{
    QList<int> result;
    for (int row = 0; row < timeline->rowCount(); ++row)
        result.append(int(timeline->index(row, 0).internalId()));
    return result;
}

bool trackMuted(TimelineItemModel *timeline, int trackId)
{
    return timeline->data(timeline->makeTrackIndexFromID(trackId), TimelineModel::IsDisabledRole).toBool();
}

/** What a read or cut addresses: a bin clip (source times only), one timeline clip, or a timeline range [start, end) on some tracks. */
struct Target
{
    enum Mode { Bin, Clip, Range } mode{Bin};
    QString binId;
    std::shared_ptr<ProjectClip> clip;
    QList<Instance> instances;
    int start{0};
    int end{0};
    QJsonArray skipped;
};

QJsonObject binClip(const QString &binId, std::shared_ptr<ProjectClip> &clip)
{
    clip = pCore->projectItemModel()->getClipByBinID(binId);
    if (!clip || !clip->statusReady()) return error(QStringLiteral("MEDIA_NOT_READY"), QStringLiteral("Bin clip is missing or still loading."));
    if (clip->clipType() == ClipType::Timeline)
        return error(QStringLiteral("SEQUENCE_PROTECTED"), QStringLiteral("Sequences have no transcript or audio of their own; address their clips."));
    return {};
}

/**
 * Exactly one of binId, clipId or range {start, end}. bin targets list their instances in the active sequence when withInstances is set.
 * A range covers clips on tracks (default: the unmuted audio tracks); a linked video clip whose audio partner is also covered is left out.
 */
QJsonObject resolveTarget(const QJsonObject &args, TimelineItemModel *timeline, const QString &invalidCode, bool withInstances, const QList<int> *tracks,
                          Target &target)
{
    const bool hasBin = args.contains(QStringLiteral("binId"));
    const bool hasClip = args.contains(QStringLiteral("clipId"));
    const bool hasRange = args.contains(QStringLiteral("range"));
    if (int(hasBin) + int(hasClip) + int(hasRange) != 1) return error(invalidCode, QStringLiteral("Pass exactly one of binId, clipId or range."));
    if (hasBin) {
        const QString id = args.value(QStringLiteral("binId")).toString();
        if (!QRegularExpression(QStringLiteral("^[0-9]+$")).match(id).hasMatch()) return error(invalidCode, QStringLiteral("Invalid binId."));
        if (auto rejected = binClip(id, target.clip); !rejected.isEmpty()) return rejected;
        target.mode = Target::Bin;
        target.binId = id;
        if (withInstances)
            for (int track : allTracks(timeline))
                for (int i = 0; i < timeline->rowCount(timeline->makeTrackIndexFromID(track)); ++i) {
                    const int id2 = int(timeline->index(i, 0, timeline->makeTrackIndexFromID(track)).internalId());
                    if (timeline->isClip(id2) && timeline->getClipBinId(id2) == id) target.instances.append(instanceOf(timeline, id2));
                }
        return {};
    }
    if (hasClip) {
        if (!integer(args, QStringLiteral("clipId"))) return error(invalidCode, QStringLiteral("Invalid clipId."));
        const int id = args.value(QStringLiteral("clipId")).toInt();
        if (!timeline->isClip(id)) return error(QStringLiteral("UNKNOWN_CLIP"), QStringLiteral("Timeline clip does not exist."));
        target.mode = Target::Clip;
        target.binId = timeline->getClipBinId(id);
        if (auto rejected = binClip(target.binId, target.clip); !rejected.isEmpty()) return rejected;
        target.instances.append(instanceOf(timeline, id));
        if (target.instances.constFirst().speed < 0)
            return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("Reversed clips cannot be mapped to source time."));
        target.start = target.instances.constFirst().position;
        target.end = target.start + target.instances.constFirst().duration;
        return {};
    }
    const auto range = args.value(QStringLiteral("range")).toObject();
    if (!args.value(QStringLiteral("range")).isObject() || !keys(range, {QStringLiteral("start"), QStringLiteral("end")}) ||
        !integer(range, QStringLiteral("start")) || !integer(range, QStringLiteral("end"), 1) ||
        range.value(QStringLiteral("start")).toInt() >= range.value(QStringLiteral("end")).toInt())
        return error(invalidCode, QStringLiteral("range needs start < end (frames, end exclusive)."));
    target.mode = Target::Range;
    target.start = range.value(QStringLiteral("start")).toInt();
    target.end = range.value(QStringLiteral("end")).toInt();
    QList<int> covered;
    if (tracks) {
        covered = *tracks;
    } else {
        for (int track : allTracks(timeline))
            if (timeline->isAudioTrack(track) && !trackMuted(timeline, track)) covered.append(track);
    }
    QList<int> ids;
    for (int track : std::as_const(covered)) {
        if (!timeline->isTrack(track)) return error(QStringLiteral("UNKNOWN_TRACK"), QStringLiteral("Track %1 does not exist.").arg(track));
        const auto index = timeline->makeTrackIndexFromID(track);
        for (int i = 0; i < timeline->rowCount(index); ++i) {
            const int id = int(timeline->index(i, 0, index).internalId());
            if (!timeline->isClip(id)) continue;
            const int position = timeline->getClipPosition(id);
            if (position >= target.end || position + timeline->getClipPlaytime(id) <= target.start) continue;
            ids.append(id);
        }
    }
    for (int id : std::as_const(ids)) {
        const int partner = timeline->getClipSplitPartner(id);
        if (partner >= 0 && ids.contains(partner) && !timeline->isAudioTrack(timeline->getClipTrackId(id))) continue;
        if (timeline->getClipState(id).first == PlaylistState::Disabled) {
            target.skipped.append(QJsonObject{{"clipId", id}, {"reason", "disabled"}});
            continue;
        }
        const auto instance = instanceOf(timeline, id);
        if (instance.speed < 0) {
            target.skipped.append(QJsonObject{{"clipId", id}, {"reason", "reversed"}});
            continue;
        }
        target.instances.append(instance);
    }
    std::sort(target.instances.begin(), target.instances.end(),
              [](const Instance &a, const Instance &b) { return a.position != b.position ? a.position < b.position : a.trackId < b.trackId; });
    return {};
}

struct Interval
{
    double start;
    double end;
};

/** A frame span [start, end). */
using Span = QPair<int, int>;

QList<Span> mergeSpans(QList<Span> spans)
{
    std::sort(spans.begin(), spans.end());
    QList<Span> result;
    for (const auto &span : std::as_const(spans)) {
        if (span.second <= span.first) continue;
        if (!result.isEmpty() && span.first <= result.constLast().second)
            result.last().second = qMax(result.constLast().second, span.second);
        else
            result.append(span);
    }
    return result;
}

/** [start, end) minus the merged spans. */
QList<Span> complementSpans(const QList<Span> &spans, int start, int end)
{
    QList<Span> result;
    int cursor = start;
    for (const auto &span : mergeSpans(spans)) {
        if (span.second <= cursor || span.first >= end) continue;
        if (span.first > cursor) result.append({cursor, span.first});
        cursor = qMax(cursor, span.second);
    }
    if (cursor < end) result.append({cursor, end});
    return result;
}

QList<Span> intersectSpans(const QList<Span> &spans, const QList<Span> &scope)
{
    QList<Span> result;
    for (const auto &a : mergeSpans(spans))
        for (const auto &b : mergeSpans(scope)) {
            const int start = qMax(a.first, b.first), end = qMin(a.second, b.second);
            if (end > start) result.append({start, end});
        }
    return mergeSpans(result);
}

struct SilenceParams
{
    double thresholdDb{-35};
    double minDuration{0.5};
    double padding{0.1};
};

QJsonObject silenceParams(const QJsonObject &object, const QString &invalidCode, SilenceParams &params)
{
    if (!number(object, QStringLiteral("thresholdDb"), -120, 0, params.thresholdDb) ||
        !number(object, QStringLiteral("minDuration"), 0.01, 3600, params.minDuration) || !number(object, QStringLiteral("padding"), 0, 60, params.padding))
        return error(invalidCode, QStringLiteral("thresholdDb is -120..0 dB, minDuration 0.01..3600 s and padding 0..60 s."));
    return {};
}

/** Runs ffmpeg silencedetect on [start, end) source seconds of a file clip's audio; detected silences are absolute source seconds. */
QJsonObject detectSilences(const std::shared_ptr<ProjectClip> &clip, double start, double end, const SilenceParams &params, QList<Interval> &detected)
{
    if (!clip->hasAudio()) return error(QStringLiteral("NO_AUDIO"), QStringLiteral("Bin clip %1 has no audio stream.").arg(clip->binId()));
    const QFileInfo file(clip->url());
    if (!file.isFile() || !file.isReadable())
        return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("Silence detection needs a clip backed by a readable local media file."));
    const QString ffmpeg = KdenliveSettings::ffmpegpath();
    if (ffmpeg.isEmpty() || !QFileInfo(ffmpeg).isExecutable())
        return error(QStringLiteral("ANALYSIS_FAILED"), QStringLiteral("ffmpeg is not configured (Settings > Configure Kdenlive > Environment)."));
    // Kdenlive's audio_index is an absolute ffmpeg stream index.
    const QString audioIndex = clip->getProducerProperty(QStringLiteral("audio_index"));
    const QString map = !audioIndex.isEmpty() && audioIndex.toInt() >= 0 ? QStringLiteral("0:%1").arg(audioIndex.toInt()) : QStringLiteral("0:a:0");
    const QStringList args{QStringLiteral("-hide_banner"),
                           QStringLiteral("-nostdin"),
                           QStringLiteral("-nostats"),
                           QStringLiteral("-ss"),
                           QString::number(start, 'f', 3),
                           QStringLiteral("-t"),
                           QString::number(end - start, 'f', 3),
                           QStringLiteral("-i"),
                           file.absoluteFilePath(),
                           QStringLiteral("-map"),
                           map,
                           QStringLiteral("-vn"),
                           QStringLiteral("-sn"),
                           QStringLiteral("-dn"),
                           QStringLiteral("-af"),
                           QStringLiteral("silencedetect=noise=%1dB:d=%2").arg(params.thresholdDb, 0, 'f', 2).arg(params.minDuration, 0, 'f', 3),
                           QStringLiteral("-f"),
                           QStringLiteral("null"),
                           QStringLiteral("-")};
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(ffmpeg, args);
    // Decoding runs far faster than real time; the timeout only catches a stuck process.
    const int timeout = int(30000 + (end - start) * 40);
    if (!process.waitForStarted(5000)) return error(QStringLiteral("ANALYSIS_FAILED"), QStringLiteral("Cannot start ffmpeg."));
    if (!process.waitForFinished(timeout)) {
        process.kill();
        process.waitForFinished(2000);
        return error(QStringLiteral("ANALYSIS_FAILED"), QStringLiteral("ffmpeg silencedetect timed out after %1 s.").arg(timeout / 1000));
    }
    const QString output = QString::fromUtf8(process.readAll());
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return error(QStringLiteral("ANALYSIS_FAILED"),
                     QStringLiteral("ffmpeg failed: %1").arg(output.trimmed().split(QLatin1Char('\n')).constLast().trimmed().left(300)));
    static const QRegularExpression marks(QStringLiteral("silence_(start|end): (-?[0-9.]+(?:e[-+]?[0-9]+)?)"));
    double open = -1;
    for (auto it = marks.globalMatch(output); it.hasNext();) {
        const auto match = it.next();
        // Times count from the -ss seek point.
        const double at = qBound(start, start + match.captured(2).toDouble(), end);
        if (match.captured(1) == QLatin1String("start")) {
            open = at;
        } else if (open >= 0) {
            if (at > open) detected.append({open, at});
            open = -1;
        }
    }
    if (open >= 0 && end > open) detected.append({open, end});
    return {};
}

/** Shrinks each silence by padding where it borders speech (not at the analysed span's edges). */
QList<Interval> padSilences(const QList<Interval> &detected, double start, double end, double padding)
{
    QList<Interval> result;
    for (const auto &silence : detected) {
        double from = qMax(silence.start, start), to = qMin(silence.end, end);
        if (from > start + 1e-6) from += padding;
        if (to < end - 1e-6) to -= padding;
        if (to - from > 1e-6) result.append({millis(from), millis(to)});
    }
    return result;
}

QList<Interval> speechBetween(const QList<Interval> &silences, double start, double end)
{
    QList<Interval> result;
    double cursor = start;
    for (const auto &silence : silences) {
        if (silence.start > cursor + 1e-6) result.append({millis(cursor), millis(silence.start)});
        cursor = qMax(cursor, silence.end);
    }
    if (end > cursor + 1e-6) result.append({millis(cursor), millis(end)});
    return result;
}

QJsonObject intervalJson(const Interval &interval, double fps, const Instance *instance)
{
    QJsonObject result{{"start", millis(interval.start)},
                       {"end", millis(interval.end)},
                       {"duration", millis(interval.end - interval.start)},
                       {"sourceStart", frameOf(interval.start, fps)},
                       {"sourceEnd", frameOf(interval.end, fps)}};
    if (instance) {
        result.insert(QStringLiteral("clipId"), instance->clipId);
        result.insert(QStringLiteral("trackId"), instance->trackId);
        result.insert(QStringLiteral("timelineStart"), instance->timeline(interval.start, fps));
        result.insert(QStringLiteral("timelineEnd"), instance->timeline(interval.end, fps));
    }
    return result;
}

QJsonObject spanJson(const Span &span, double fps)
{
    return {{"timelineStart", span.first},
            {"timelineEnd", span.second},
            {"duration", span.second - span.first},
            {"start", millis(span.first / fps)},
            {"end", millis(span.second / fps)}};
}

/** Per-instance silence analysis of a target, within [start, end) of the timeline for range targets. */
struct SilenceRun
{
    const Instance *instance{nullptr};
    double start{0};
    double end{0};
    QList<Interval> detected;
    QList<Interval> silences;
};

QJsonObject analyse(Target &target, const SilenceParams &params, double fps, QList<SilenceRun> &runs, const QJsonObject *sourceRange)
{
    double total = 0;
    if (target.mode == Target::Bin && target.instances.isEmpty()) {
        SilenceRun run;
        run.start = 0;
        run.end = target.clip->duration().seconds();
        if (sourceRange) {
            run.start = sourceRange->value(QStringLiteral("start")).toDouble();
            run.end = qMin(run.end, sourceRange->value(QStringLiteral("end")).toDouble());
        }
        runs.append(run);
    } else {
        for (const auto &instance : std::as_const(target.instances)) {
            if (instance.speed < 0) continue;
            int from = instance.position, to = instance.position + instance.duration;
            if (target.mode == Target::Range) {
                from = qMax(from, target.start);
                to = qMin(to, target.end);
            }
            auto clip = pCore->projectItemModel()->getClipByBinID(instance.binId);
            if (!clip || !clip->hasAudio() || to <= from) {
                if (target.mode != Target::Range) return error(QStringLiteral("NO_AUDIO"), QStringLiteral("The clip has no audio."));
                target.skipped.append(QJsonObject{{"clipId", instance.clipId}, {"reason", clip ? "no audio" : "missing bin clip"}});
                continue;
            }
            SilenceRun run;
            run.instance = &instance;
            run.start = instance.source(from, fps);
            run.end = instance.source(to, fps);
            runs.append(run);
        }
    }
    if (runs.isEmpty())
        return error(QStringLiteral("NO_AUDIO"), QStringLiteral("No clip with audio in the range to measure; check trackIds (audio tracks, not muted)."));
    for (const auto &run : std::as_const(runs))
        total += run.end - run.start;
    if (total > maxAnalysisSeconds)
        return error(QStringLiteral("TOO_LONG"),
                     QStringLiteral("Silence detection covers at most 2 hours of audio per call (%1 s requested); narrow it with range or sourceRange.")
                         .arg(int(total)));
    for (auto &run : runs) {
        if (run.end - run.start <= 0) continue;
        auto clip = run.instance ? pCore->projectItemModel()->getClipByBinID(run.instance->binId) : target.clip;
        if (auto rejected = detectSilences(clip, run.start, run.end, params, run.detected); !rejected.isEmpty()) return rejected;
        run.silences = padSilences(run.detected, run.start, run.end, params.padding);
    }
    return {};
}

/** Timeline frames that are silent: per instance its padded silences; for a range, frames where no analysed clip has speech. */
QList<Span> timelineSilences(const Target &target, const QList<SilenceRun> &runs, const SilenceParams &params, double fps)
{
    QList<Span> result;
    if (target.mode == Target::Range) {
        QList<Span> speech;
        for (const auto &run : runs)
            for (const auto &interval : speechBetween(run.silences, run.start, run.end))
                speech.append({run.instance->timeline(interval.start, fps), run.instance->timeline(interval.end, fps)});
        const int shortest = qMax(1, frameOf(params.minDuration - 2 * params.padding, fps));
        for (const auto &span : complementSpans(speech, target.start, target.end))
            if (span.second - span.first >= shortest) result.append(span);
        return result;
    }
    for (const auto &run : runs)
        for (const auto &silence : run.silences)
            if (run.instance) result.append({run.instance->timeline(silence.start, fps), run.instance->timeline(silence.end, fps)});
    return mergeSpans(result);
}

QString srtTime(double seconds)
{
    const qint64 total = std::llround(qMax(0.0, seconds) * 1000);
    return QStringLiteral("%1:%2:%3,%4")
        .arg(total / 3600000, 2, 10, QLatin1Char('0'))
        .arg(total / 60000 % 60, 2, 10, QLatin1Char('0'))
        .arg(total / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(total % 1000, 3, 10, QLatin1Char('0'));
}

/** Words of a transcript visible through an instance's source window (or all words for a bin read), as index range [first, last]. */
QPair<int, int> visibleWords(const QList<Word> &words, double start, double end)
{
    int first = -1, last = -2;
    for (int i = 0; i < words.size(); ++i) {
        const auto &word = words.at(i);
        const bool inside = word.end > word.start ? word.end > start && word.start < end : word.start >= start && word.start < end;
        if (!inside) continue;
        if (first < 0) first = i;
        last = i;
    }
    return {first, last};
}

/** Assembles words, segments, gaps and text lines of one view (a bin clip or one timeline instance). */
struct TranscriptView
{
    QJsonArray words;
    QJsonArray segments;
    QJsonArray gaps;
    QStringList lines;
    QStringList cues;
    int count{0};
};

void addView(const Transcript &transcript, const Instance *instance, double start, double end, const Target &target, double minPause, double fps,
             TranscriptView &view)
{
    const auto &words = transcript.words;
    const auto [first, last] = visibleWords(words, start, end);
    const bool range = target.mode == Target::Range;
    const auto timelineOf = [&](double from, double to, QJsonObject &item) {
        if (!instance) return true;
        const int a = instance->timeline(from, fps), b = instance->timeline(to, fps);
        if (range && !(b > a ? b > target.start && a < target.end : a >= target.start && a < target.end)) return false;
        item.insert(QStringLiteral("clipId"), instance->clipId);
        item.insert(QStringLiteral("trackId"), instance->trackId);
        if (range) item.insert(QStringLiteral("binId"), instance->binId);
        item.insert(QStringLiteral("timelineStart"), a);
        item.insert(QStringLiteral("timelineEnd"), b);
        return true;
    };
    QList<int> shown;
    for (int i = first; i >= 0 && i <= last; ++i) {
        const auto &word = words.at(i);
        QJsonObject item{{"index", i},
                         {"text", word.text},
                         {"start", word.start},
                         {"end", word.end},
                         {"sourceStart", frameOf(word.start, fps)},
                         {"sourceEnd", frameOf(word.end, fps)}};
        if (word.confidence >= 0) item.insert(QStringLiteral("confidence"), word.confidence);
        if (word.segment >= 0) item.insert(QStringLiteral("segment"), word.segment);
        if (word.interpolated) item.insert(QStringLiteral("interpolated"), true);
        if (!timelineOf(word.start, word.end, item)) continue;
        if (instance && (word.start < start || word.end > end)) item.insert(QStringLiteral("clipped"), true);
        view.words.append(item);
        shown.append(i);
    }
    view.count += int(shown.size());
    // Pauses: before the first word, between words and after the last one, inside the window.
    double cursor = start;
    int previous = -1;
    const auto gap = [&](double from, double to, int after, int before) {
        if (to - from <= minPause) return;
        QJsonObject item{{"start", millis(from)},         {"end", millis(to)},  {"duration", millis(to - from)}, {"sourceStart", frameOf(from, fps)},
                         {"sourceEnd", frameOf(to, fps)}, {"afterWord", after}, {"beforeWord", before}};
        if (timelineOf(from, to, item)) view.gaps.append(item);
    };
    for (int i = first; i >= 0 && i <= last; ++i) {
        gap(cursor, words.at(i).start, previous, i);
        cursor = qMax(cursor, words.at(i).end);
        previous = i;
    }
    gap(cursor, end, previous, -1);
    // Segments over the shown words (contiguous runs of indexes).
    int runStart = 0;
    for (int k = 0; k < shown.size(); ++k) {
        if (k + 1 < shown.size() && shown.at(k + 1) == shown.at(k) + 1) continue;
        for (const auto &segment : segmentsOf(words, shown.at(runStart), shown.at(k), minPause)) {
            QStringList text;
            for (int i = segment.first; i <= segment.last; ++i)
                text << words.at(i).text;
            const double from = words.at(segment.first).start, to = words.at(segment.last).end;
            QJsonObject item{{"index", int(view.segments.size())},
                             {"text", text.join(QLatin1Char(' '))},
                             {"start", from},
                             {"end", to},
                             {"sourceStart", frameOf(from, fps)},
                             {"sourceEnd", frameOf(to, fps)},
                             {"firstWord", segment.first},
                             {"lastWord", segment.last}};
            timelineOf(from, to, item);
            view.segments.append(item);
            view.lines << item.value(QStringLiteral("text")).toString();
            const double cueStart = instance ? item.value(QStringLiteral("timelineStart")).toInt() / fps : from;
            const double cueEnd = instance ? item.value(QStringLiteral("timelineEnd")).toInt() / fps : to;
            view.cues << QStringLiteral("%1\n%2 --> %3\n%4\n").arg(view.cues.size() + 1).arg(srtTime(cueStart), srtTime(cueEnd), item.value("text").toString());
        }
        runStart = k + 1;
    }
}

} // namespace

struct LiveBridge::TranscriptionJob
{
    QString binId;
    QString engine;
    QString model;
    QString language;
    QString url;
    QString state{QStringLiteral("running")};
    QString error;
    QString requestId;
    Caller caller;
    QPointer<KdenliveDoc> document;
    QPointer<QProcess> process;
    QByteArray output;
    QString log;
    int progress{0};
    double duration{0};
    int wordCount{0};
    int undoIndex{-1};
    qint64 started{0};
    qint64 finished{0};
    Transcript result;
};

QJsonObject LiveBridge::transcript(const QJsonObject &arguments)
{
    if (!bind()) return error(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    const auto invalid = [](const QString &message) { return error(QStringLiteral("INVALID_ARGUMENTS"), message); };
    if (!keys(arguments, {QStringLiteral("binId"), QStringLiteral("clipId"), QStringLiteral("range"), QStringLiteral("trackIds"), QStringLiteral("format"),
                          QStringLiteral("minPause")}))
        return invalid(QStringLiteral("Unknown argument."));
    const QString format = arguments.value(QStringLiteral("format")).toString(QStringLiteral("words"));
    if (!QStringList{QStringLiteral("words"), QStringLiteral("segments"), QStringLiteral("text"), QStringLiteral("srt")}.contains(format))
        return invalid(QStringLiteral("format is words, segments, text or srt."));
    double minPause = 0.5;
    if (!number(arguments, QStringLiteral("minPause"), 0, 3600, minPause)) return invalid(QStringLiteral("minPause is 0..3600 seconds."));
    QList<int> tracks;
    if (arguments.contains(QStringLiteral("trackIds"))) {
        if (!arguments.contains(QStringLiteral("range"))) return invalid(QStringLiteral("trackIds only narrows a range."));
        for (const auto &item : arguments.value(QStringLiteral("trackIds")).toArray())
            tracks.append(item.toInt(-1));
        if (tracks.isEmpty() || tracks.contains(-1)) return invalid(QStringLiteral("trackIds lists track ids."));
    }
    Target target;
    if (auto rejected = resolveTarget(arguments, m_timeline, QStringLiteral("INVALID_ARGUMENTS"), false,
                                      arguments.contains(QStringLiteral("trackIds")) ? &tracks : nullptr, target);
        !rejected.isEmpty())
        return rejected;
    const double fps = pCore->getCurrentFps();
    TranscriptView view;
    QJsonObject result{{"ok", true}, {"format", format}, {"minPause", minPause}};
    if (target.mode != Target::Range) {
        Transcript transcript;
        if (!loadTranscript(target.clip, transcript))
            return error(QStringLiteral("NO_TRANSCRIPT"),
                         QStringLiteral("Bin clip %1 has no transcript; use desktop_transcript_import or desktop_transcribe.").arg(target.binId));
        const Instance *instance = target.mode == Target::Clip ? &target.instances.constFirst() : nullptr;
        const double start = instance ? instance->windowStart(fps) : 0;
        const double end = instance ? instance->windowEnd(fps) : target.clip->duration().seconds();
        addView(transcript, instance, start, end, target, minPause, fps, view);
        result.insert(QStringLiteral("target"), instance ? QStringLiteral("clip") : QStringLiteral("bin"));
        result.insert(QStringLiteral("binId"), target.binId);
        if (instance) result.insert(QStringLiteral("clipId"), instance->clipId);
        result.insert(QStringLiteral("language"), transcript.language);
        result.insert(QStringLiteral("engine"), transcript.engine);
        result.insert(QStringLiteral("origin"), transcript.origin);
        result.insert(QStringLiteral("wordCount"), int(transcript.words.size()));
    } else {
        QJsonArray clips;
        for (const auto &instance : std::as_const(target.instances)) {
            auto clip = pCore->projectItemModel()->getClipByBinID(instance.binId);
            Transcript transcript;
            if (!clip || !loadTranscript(clip, transcript)) {
                target.skipped.append(QJsonObject{{"clipId", instance.clipId}, {"reason", "no transcript"}});
                continue;
            }
            addView(transcript, &instance, instance.windowStart(fps), instance.windowEnd(fps), target, minPause, fps, view);
            clips.append(QJsonObject{{"clipId", instance.clipId},
                                     {"trackId", instance.trackId},
                                     {"binId", instance.binId},
                                     {"language", transcript.language},
                                     {"engine", transcript.engine},
                                     {"wordCount", int(transcript.words.size())}});
        }
        if (clips.isEmpty()) return error(QStringLiteral("NO_TRANSCRIPT"), QStringLiteral("No clip in the range has a transcript."));
        result.insert(QStringLiteral("target"), QStringLiteral("range"));
        result.insert(QStringLiteral("range"), QJsonObject{{"start", target.start}, {"end", target.end}});
        result.insert(QStringLiteral("clips"), clips);
        result.insert(QStringLiteral("skipped"), target.skipped);
    }
    result.insert(QStringLiteral("count"), view.count);
    result.insert(QStringLiteral("gaps"), view.gaps);
    if (format == QLatin1String("words")) result.insert(QStringLiteral("words"), view.words);
    if (format == QLatin1String("segments")) result.insert(QStringLiteral("segments"), view.segments);
    if (format == QLatin1String("text")) result.insert(QStringLiteral("text"), view.lines.join(QLatin1Char('\n')));
    if (format == QLatin1String("srt")) result.insert(QStringLiteral("text"), view.cues.join(QLatin1Char('\n')));
    return result;
}

QJsonObject LiveBridge::silenceDetect(const QJsonObject &arguments)
{
    if (!bind()) return error(QStringLiteral("NOT_READY"), QStringLiteral("No fully loaded active timeline."));
    const auto invalid = [](const QString &message) { return error(QStringLiteral("INVALID_ARGUMENTS"), message); };
    if (!keys(arguments, {QStringLiteral("binId"), QStringLiteral("clipId"), QStringLiteral("range"), QStringLiteral("trackIds"), QStringLiteral("sourceRange"),
                          QStringLiteral("thresholdDb"), QStringLiteral("minDuration"), QStringLiteral("padding")}))
        return invalid(QStringLiteral("Unknown argument."));
    SilenceParams params;
    if (auto rejected = silenceParams(arguments, QStringLiteral("INVALID_ARGUMENTS"), params); !rejected.isEmpty()) return rejected;
    QList<int> tracks;
    if (arguments.contains(QStringLiteral("trackIds"))) {
        if (!arguments.contains(QStringLiteral("range"))) return invalid(QStringLiteral("trackIds only narrows a range."));
        for (const auto &item : arguments.value(QStringLiteral("trackIds")).toArray())
            tracks.append(item.toInt(-1));
        if (tracks.isEmpty() || tracks.contains(-1)) return invalid(QStringLiteral("trackIds lists track ids."));
    }
    Target target;
    if (auto rejected = resolveTarget(arguments, m_timeline, QStringLiteral("INVALID_ARGUMENTS"), false,
                                      arguments.contains(QStringLiteral("trackIds")) ? &tracks : nullptr, target);
        !rejected.isEmpty())
        return rejected;
    QJsonObject sourceRange;
    if (arguments.contains(QStringLiteral("sourceRange"))) {
        sourceRange = arguments.value(QStringLiteral("sourceRange")).toObject();
        double from = -1, to = -1;
        if (target.mode != Target::Bin || !keys(sourceRange, {QStringLiteral("start"), QStringLiteral("end")}) ||
            !number(sourceRange, QStringLiteral("start"), 0, 1e9, from) || !number(sourceRange, QStringLiteral("end"), 0, 1e9, to) || from < 0 || to <= from)
            return invalid(QStringLiteral("sourceRange {start, end} (seconds, start < end) narrows a binId read."));
    }
    if (target.mode != Target::Range && !target.clip->hasAudio())
        return error(QStringLiteral("NO_AUDIO"), QStringLiteral("Bin clip %1 has no audio stream.").arg(target.binId));
    const double fps = pCore->getCurrentFps();
    QList<SilenceRun> runs;
    if (auto rejected = analyse(target, params, fps, runs, sourceRange.isEmpty() ? nullptr : &sourceRange); !rejected.isEmpty()) return rejected;
    QJsonObject result{{"ok", true}, {"thresholdDb", params.thresholdDb}, {"minDuration", params.minDuration}, {"padding", params.padding}};
    const auto listOf = [fps](const QList<Interval> &intervals, const Instance *instance) {
        QJsonArray list;
        for (const auto &interval : intervals)
            list.append(intervalJson(interval, fps, instance));
        return list;
    };
    if (target.mode != Target::Range) {
        const auto &run = runs.constFirst();
        QJsonArray silences;
        for (int i = 0; i < run.silences.size(); ++i)
            silences.append(intervalJson(run.silences.at(i), fps, run.instance));
        // The unpadded detection, matched by overlap.
        for (int i = 0; i < silences.size(); ++i) {
            auto item = silences.at(i).toObject();
            for (const auto &detected : run.detected)
                if (detected.start <= run.silences.at(i).start + 1e-6 && detected.end >= run.silences.at(i).end - 1e-6) {
                    item.insert(QStringLiteral("detectedStart"), millis(detected.start));
                    item.insert(QStringLiteral("detectedEnd"), millis(detected.end));
                }
            silences.replace(i, item);
        }
        result.insert(QStringLiteral("target"), run.instance ? QStringLiteral("clip") : QStringLiteral("bin"));
        result.insert(QStringLiteral("binId"), target.binId);
        if (run.instance) result.insert(QStringLiteral("clipId"), run.instance->clipId);
        result.insert(QStringLiteral("analyzed"), QJsonObject{{"start", millis(run.start)}, {"end", millis(run.end)}});
        result.insert(QStringLiteral("silences"), silences);
        result.insert(QStringLiteral("speech"), listOf(speechBetween(run.silences, run.start, run.end), run.instance));
        return result;
    }
    QJsonArray clips;
    for (const auto &run : std::as_const(runs))
        clips.append(QJsonObject{{"clipId", run.instance->clipId},
                                 {"trackId", run.instance->trackId},
                                 {"binId", run.instance->binId},
                                 {"analyzed", QJsonObject{{"start", millis(run.start)}, {"end", millis(run.end)}}},
                                 {"silences", listOf(run.silences, run.instance)}});
    const auto silent = timelineSilences(target, runs, params, fps);
    QJsonArray silences, speech;
    for (const auto &span : silent)
        silences.append(spanJson(span, fps));
    for (const auto &span : complementSpans(silent, target.start, target.end))
        speech.append(spanJson(span, fps));
    result.insert(QStringLiteral("target"), QStringLiteral("range"));
    result.insert(QStringLiteral("range"), QJsonObject{{"start", target.start}, {"end", target.end}});
    result.insert(QStringLiteral("clips"), clips);
    result.insert(QStringLiteral("skipped"), target.skipped);
    result.insert(QStringLiteral("silences"), silences);
    result.insert(QStringLiteral("speech"), speech);
    return result;
}

QJsonObject LiveBridge::executeTranscript(const QJsonObject &command, bool &handled)
{
    handled = true;
    const QString type = command.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("transcript_import")) return importTranscript(command);
    if (type == QLatin1String("range_cut")) return rangeCut(command);
    if (type == QLatin1String("transcribe")) return startTranscription(command);
    handled = false;
    return {};
}

QJsonObject LiveBridge::importTranscript(const QJsonObject &command)
{
    const auto invalid = [](const QString &message) { return error(QStringLiteral("INVALID_COMMAND"), message); };
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("binId"), QStringLiteral("format"), QStringLiteral("text"), QStringLiteral("language"),
                        QStringLiteral("engine"), QStringLiteral("append")}))
        return invalid(QStringLiteral("transcript_import takes binId, format, text, language, engine and append."));
    const QString binId = command.value(QStringLiteral("binId")).toString();
    const QString format = command.value(QStringLiteral("format")).toString();
    bool append = false;
    if (!QRegularExpression(QStringLiteral("^[0-9]+$")).match(binId).hasMatch() ||
        !QStringList{QStringLiteral("json"), QStringLiteral("srt"), QStringLiteral("vtt"), QStringLiteral("whisper")}.contains(format) ||
        !command.value(QStringLiteral("text")).isString() || command.value(QStringLiteral("text")).toString().trimmed().isEmpty() ||
        (command.contains(QStringLiteral("language")) && !command.value(QStringLiteral("language")).isString()) ||
        (command.contains(QStringLiteral("engine")) && !command.value(QStringLiteral("engine")).isString()) ||
        command.value(QStringLiteral("language")).toString().size() > 64 || command.value(QStringLiteral("engine")).toString().size() > 64 ||
        !optionalBool(command, QStringLiteral("append"), append))
        return invalid(QStringLiteral("Expected binId, format (json, srt, vtt or whisper) and non-empty text."));
    std::shared_ptr<ProjectClip> clip;
    if (auto rejected = binClip(binId, clip); !rejected.isEmpty()) return rejected;
    if (!clip->hasAudio()) return error(QStringLiteral("NO_AUDIO"), QStringLiteral("Bin clip %1 has no audio stream to transcribe.").arg(binId));
    Transcript transcript;
    if (auto rejected = parseTranscript(format, command.value(QStringLiteral("text")).toString(), transcript); !rejected.isEmpty()) return rejected;
    if (transcript.words.isEmpty()) return invalidTranscript(QStringLiteral("The transcript has no words."));
    const double duration = clip->duration().seconds();
    for (const auto &word : std::as_const(transcript.words))
        if (clip->hasLimitedDuration() && word.start > duration + 1.0)
            return invalidTranscript(
                QStringLiteral("Word '%1' at %2 s lies beyond the clip's %3 s; is this the right clip?").arg(word.text).arg(word.start).arg(millis(duration)));
    Transcript previous;
    const bool replaced = loadTranscript(clip, previous);
    if (append && replaced) {
        int offset = 0;
        for (const auto &word : std::as_const(previous.words))
            offset = qMax(offset, word.segment + 1);
        for (auto &word : transcript.words)
            if (word.segment >= 0) word.segment += offset;
        // Appended words without segments would hide the stored segmentation; give each run its own segment.
        auto words = previous.words + transcript.words;
        std::stable_sort(words.begin(), words.end(), [](const Word &a, const Word &b) { return a.start < b.start; });
        if (words.size() > maxWords) return invalidTranscript(QStringLiteral("At most %1 words per clip.").arg(maxWords));
        if (transcript.language.isEmpty()) transcript.language = previous.language;
        if (transcript.engine.isEmpty()) transcript.engine = previous.engine;
        transcript.words = words;
    }
    if (command.contains(QStringLiteral("language"))) transcript.language = command.value(QStringLiteral("language")).toString();
    if (command.contains(QStringLiteral("engine"))) transcript.engine = command.value(QStringLiteral("engine")).toString();
    if (transcript.engine.isEmpty()) transcript.engine = format == QLatin1String("whisper") ? QStringLiteral("whisper") : QStringLiteral("import");
    if (auto rejected = storeTranscript(m_document, clip, transcript, QStringLiteral("Import transcript")); !rejected.isEmpty()) return rejected;
    int interpolated = 0;
    for (const auto &word : std::as_const(transcript.words))
        interpolated += word.interpolated ? 1 : 0;
    const int segments = int(segmentsOf(transcript.words, 0, int(transcript.words.size()) - 1, 0.5).size());
    return {{"ok", true},
            {"binId", binId},
            {"wordCount", int(transcript.words.size())},
            {"segmentCount", segments},
            {"interpolatedWords", interpolated},
            {"language", transcript.language},
            {"engine", transcript.engine},
            {"end", transcript.words.constLast().end},
            {"replaced", replaced && !append},
            {"appended", append && replaced},
            {"historySummary", QJsonObject{{"wordCount", int(transcript.words.size())}, {"language", transcript.language}}}};
}

QJsonObject LiveBridge::rangeCut(const QJsonObject &command)
{
    const auto invalid = [](const QString &message) { return error(QStringLiteral("INVALID_COMMAND"), message); };
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("ranges"), QStringLiteral("words"), QStringLiteral("silences"), QStringLiteral("binId"),
                        QStringLiteral("clipId"), QStringLiteral("range"), QStringLiteral("trackIds"), QStringLiteral("allTracks"), QStringLiteral("mode"),
                        QStringLiteral("keep"), QStringLiteral("padding"), QStringLiteral("minGap"), QStringLiteral("addMarkers"), QStringLiteral("dryRun")}))
        return invalid(QStringLiteral("Unknown range_cut field."));
    const bool byRanges = command.contains(QStringLiteral("ranges"));
    const bool byWords = command.contains(QStringLiteral("words"));
    const bool bySilences = command.contains(QStringLiteral("silences"));
    if (int(byRanges) + int(byWords) + int(bySilences) != 1) return invalid(QStringLiteral("Pass exactly one of ranges, words or silences."));
    const QString mode = command.value(QStringLiteral("mode")).toString(QStringLiteral("extract"));
    if (mode != QLatin1String("extract") && mode != QLatin1String("lift")) return invalid(QStringLiteral("mode is extract or lift."));
    const bool extract = mode == QLatin1String("extract");
    bool keep = false, dryRun = false, allTracksFlag = false;
    if (!optionalBool(command, QStringLiteral("keep"), keep) || !optionalBool(command, QStringLiteral("dryRun"), dryRun) ||
        !optionalBool(command, QStringLiteral("allTracks"), allTracksFlag))
        return invalid(QStringLiteral("keep, dryRun and allTracks are booleans."));
    if ((command.contains(QStringLiteral("padding")) && !integer(command, QStringLiteral("padding"))) ||
        (command.contains(QStringLiteral("minGap")) && !integer(command, QStringLiteral("minGap"))))
        return invalid(QStringLiteral("padding and minGap are frame counts."));
    const int padding = command.value(QStringLiteral("padding")).toInt(0);
    const int minGap = qMax(1, command.value(QStringLiteral("minGap")).toInt(1));
    int category = -1;
    const bool markers = command.contains(QStringLiteral("addMarkers"));
    if (markers)
        if (auto rejected = markerCategoryFor(command.value(QStringLiteral("addMarkers")), category); !rejected.isEmpty()) return rejected;
    // The tracks to cut: trackIds, or every unlocked track (the default).
    if (command.contains(QStringLiteral("trackIds")) && allTracksFlag) return invalid(QStringLiteral("Pass trackIds or allTracks: true, not both."));
    if (command.contains(QStringLiteral("allTracks")) && !allTracksFlag && !command.contains(QStringLiteral("trackIds")))
        return invalid(QStringLiteral("allTracks: false needs trackIds."));
    const bool everyTrack = !command.contains(QStringLiteral("trackIds"));
    QList<int> tracks;
    if (!everyTrack) {
        const auto list = command.value(QStringLiteral("trackIds")).toArray();
        if (list.isEmpty() || list.size() > 64) return invalid(QStringLiteral("trackIds must list 1 to 64 tracks."));
        for (const auto &item : list) {
            if (!item.isDouble() || item.toDouble() < 0 || std::floor(item.toDouble()) != item.toDouble() || tracks.contains(item.toInt()))
                return invalid(QStringLiteral("trackIds must be distinct track ids."));
            tracks.append(item.toInt());
        }
        for (int track : std::as_const(tracks))
            if (auto rejected = editableTrack(track); !rejected.isEmpty()) return rejected;
    } else {
        tracks = unlockedTracks();
    }
    const double fps = pCore->getCurrentFps();
    const int duration = m_timeline->duration();
    QList<Span> selected;
    QList<Span> scope;
    QString source;
    Target target;
    if (byRanges) {
        source = QStringLiteral("ranges");
        if (command.contains(QStringLiteral("binId")) || command.contains(QStringLiteral("clipId")))
            return invalid(QStringLiteral("ranges are timeline frames; binId and clipId address words or silences."));
        const auto list = command.value(QStringLiteral("ranges")).toArray();
        if (!command.value(QStringLiteral("ranges")).isArray() || list.isEmpty() || list.size() > maxCutRanges)
            return invalid(QStringLiteral("ranges lists 1 to %1 {start, end} frame spans.").arg(maxCutRanges));
        for (const auto &item : list) {
            const auto span = item.toObject();
            if (!item.isObject() || !keys(span, {QStringLiteral("start"), QStringLiteral("end")}) || !integer(span, QStringLiteral("start")) ||
                !integer(span, QStringLiteral("end"), 1) || span.value(QStringLiteral("start")).toInt() >= span.value(QStringLiteral("end")).toInt())
                return invalid(QStringLiteral("Each range needs start < end (frames, end exclusive)."));
            selected.append({span.value(QStringLiteral("start")).toInt(), span.value(QStringLiteral("end")).toInt()});
        }
        if (command.contains(QStringLiteral("range"))) {
            if (auto rejected = resolveTarget(QJsonObject{{"range", command.value(QStringLiteral("range"))}}, m_timeline, QStringLiteral("INVALID_COMMAND"),
                                              false, nullptr, target);
                !rejected.isEmpty())
                return rejected;
            scope.append({target.start, target.end});
        } else {
            scope.append({0, duration});
        }
    } else {
        QJsonObject address;
        for (const auto &key : {QStringLiteral("binId"), QStringLiteral("clipId"), QStringLiteral("range")})
            if (command.contains(key)) address.insert(key, command.value(key));
        if (byWords && address.contains(QStringLiteral("range"))) return invalid(QStringLiteral("words are addressed on a binId or clipId."));
        // Silences over a range are measured on the cut tracks' unmuted audio tracks.
        QList<int> analysis;
        for (int track : std::as_const(tracks))
            if (m_timeline->isAudioTrack(track) && !trackMuted(m_timeline, track)) analysis.append(track);
        if (auto rejected = resolveTarget(address, m_timeline, QStringLiteral("INVALID_COMMAND"), true, &analysis, target); !rejected.isEmpty())
            return rejected;
        if (target.mode == Target::Bin) {
            for (const auto &instance : std::as_const(target.instances))
                if (instance.speed < 0) return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("Reversed clips cannot be mapped to source time."));
            if (target.instances.isEmpty())
                return error(QStringLiteral("UNKNOWN_CLIP"), QStringLiteral("Bin clip %1 has no clip in the active sequence.").arg(target.binId));
        }
        if (target.mode == Target::Range)
            scope.append({target.start, target.end});
        else
            for (const auto &instance : std::as_const(target.instances))
                scope.append({instance.position, instance.position + instance.duration});
        if (byWords) {
            source = QStringLiteral("words");
            Transcript transcript;
            if (!loadTranscript(target.clip, transcript))
                return error(QStringLiteral("NO_TRANSCRIPT"), QStringLiteral("Bin clip %1 has no transcript.").arg(target.binId));
            const auto list = command.value(QStringLiteral("words")).toArray();
            if (!command.value(QStringLiteral("words")).isArray() || list.isEmpty() || list.size() > maxCutRanges)
                return invalid(QStringLiteral("words lists 1 to %1 {from, to} word index spans.").arg(maxCutRanges));
            for (const auto &item : list) {
                const auto span = item.toObject();
                if (!item.isObject() || !keys(span, {QStringLiteral("from"), QStringLiteral("to")}) || !integer(span, QStringLiteral("from")) ||
                    !integer(span, QStringLiteral("to")) || span.value(QStringLiteral("from")).toInt() > span.value(QStringLiteral("to")).toInt())
                    return invalid(QStringLiteral("Each words entry needs from <= to (word indexes, inclusive)."));
                const int from = span.value(QStringLiteral("from")).toInt(), to = span.value(QStringLiteral("to")).toInt();
                if (to >= transcript.words.size())
                    return invalid(QStringLiteral("Word index %1 is out of range; the transcript has %2 words.").arg(to).arg(transcript.words.size()));
                const double start = transcript.words.at(from).start, end = transcript.words.at(to).end;
                for (const auto &instance : std::as_const(target.instances)) {
                    const double windowStart = instance.windowStart(fps), windowEnd = instance.windowEnd(fps);
                    if (end <= windowStart || start >= windowEnd) continue;
                    selected.append({instance.timeline(qMax(start, windowStart), fps), instance.timeline(qMin(end, windowEnd), fps)});
                }
            }
        } else {
            source = QStringLiteral("silences");
            SilenceParams params;
            const auto value = command.value(QStringLiteral("silences"));
            if (value.isObject()) {
                if (!keys(value.toObject(), {QStringLiteral("thresholdDb"), QStringLiteral("minDuration"), QStringLiteral("padding")}))
                    return invalid(QStringLiteral("silences takes thresholdDb, minDuration and padding."));
                if (auto rejected = silenceParams(value.toObject(), QStringLiteral("INVALID_COMMAND"), params); !rejected.isEmpty()) return rejected;
            } else if (value != QJsonValue(true)) {
                return invalid(QStringLiteral("silences is true or {thresholdDb, minDuration, padding}."));
            }
            QList<SilenceRun> runs;
            if (auto rejected = analyse(target, params, fps, runs, nullptr); !rejected.isEmpty()) return rejected;
            selected = timelineSilences(target, runs, params, fps);
        }
    }
    // Final ranges: merged, inverted for keep, shrunk by padding, short ones dropped.
    QList<Span> removed = keep ? QList<Span>() : intersectSpans(selected, scope);
    if (keep)
        for (const auto &span : mergeSpans(scope))
            removed += complementSpans(intersectSpans(selected, {span}), span.first, span.second);
    QList<Span> ranges;
    for (const auto &span : mergeSpans(removed)) {
        const Span padded{span.first + padding, span.second - padding};
        if (padded.second - padded.first >= minGap) ranges.append(padded);
    }
    if (ranges.size() > maxCutRanges) return invalid(QStringLiteral("The cut has %1 ranges; at most %2 per call.").arg(ranges.size()).arg(maxCutRanges));
    int removedFrames = 0;
    QJsonArray rangeList, summaryRanges;
    for (const auto &span : std::as_const(ranges)) {
        removedFrames += span.second - span.first;
        rangeList.append(QJsonObject{{"start", span.first}, {"end", span.second}, {"duration", span.second - span.first}});
        if (summaryRanges.size() < 50) summaryRanges.append(QJsonArray{span.first, span.second});
    }
    // Where each cut lands afterwards: a point guide at the join (extract) or a range guide over the gap (lift).
    QList<QPair<int, int>> markerSpans;
    int shift = 0;
    for (const auto &span : std::as_const(ranges)) {
        markerSpans.append(extract ? QPair<int, int>{span.first - shift, 0} : QPair<int, int>{span.first, span.second - span.first});
        if (extract) shift += span.second - span.first;
    }
    QJsonObject result{{"ok", true},
                       {"mode", mode},
                       {"source", source},
                       {"keep", keep},
                       {"trackIds", idArray(tracks)},
                       {"removedRanges", rangeList},
                       {"removedFrames", removedFrames}};
    if (dryRun || ranges.isEmpty()) {
        result.insert(QStringLiteral("dryRun"), dryRun);
        result.insert(QStringLiteral("changed"), false);
        result.insert(QStringLiteral("newDuration"), extract && everyTrack ? qMax(0, duration - removedFrames) : duration);
        return result;
    }
    m_timeline->requestClearSelection();
    const auto before = allClipIds();
    QHash<int, QPair<int, int>> placement;
    for (int id : before)
        placement.insert(id, {m_timeline->getClipPosition(id), m_timeline->getClipPlaytime(id)});
    Fun undo = [] { return true; };
    Fun redo = [] { return true; };
    const QVector<int> trackVector(tracks.cbegin(), tracks.cend());
    bool ok = true;
    // Last range first, so earlier ranges keep their frame positions.
    for (auto it = ranges.crbegin(); ok && it != ranges.crend(); ++it) {
        const QPoint zone(it->first, it->second);
        ok = runStep(undo, redo, [&](Fun &u, Fun &r) { return rippleRemove(trackVector, zone, !extract, u, r); });
        if (ok && extract && everyTrack) ok = runStep(undo, redo, [&](Fun &u, Fun &r) { return rippleGuides(zone, u, r); });
    }
    QJsonArray markersAdded;
    if (ok && markers) {
        auto guides = m_timeline->getGuideModel();
        for (int i = 0; ok && i < markerSpans.size(); ++i) {
            const auto &[position, length] = markerSpans.at(i);
            const QString comment = extract ? QStringLiteral("Cut: %1 frames removed").arg(ranges.at(i).second - ranges.at(i).first)
                                            : QStringLiteral("Lifted %1 frames").arg(length);
            ok = runStep(undo, redo, [&](Fun &u, Fun &r) {
                return length > 0 ? guides->addRangeMarker(GenTime(position, fps), GenTime(length, fps), comment, category, u, r)
                                  : guides->addMarker(GenTime(position, fps), comment, category, u, r);
            });
            markersAdded.append(QJsonObject{{"position", position}, {"duration", length}, {"comment", comment}, {"category", category}});
        }
    }
    if (!ok) {
        undo();
        return error(QStringLiteral("EDIT_REJECTED"), QStringLiteral("Native lift/extract was rejected."));
    }
    const QString label = ranges.size() == 1 ? (extract ? QStringLiteral("Remove range") : QStringLiteral("Lift range"))
                                             : (extract ? QStringLiteral("Remove %1 ranges") : QStringLiteral("Lift %1 ranges")).arg(ranges.size());
    pCore->pushUndo(undo, redo, label);
    const auto after = allClipIds();
    QSet<int> affected = (before - after) + (after - before);
    for (int id : after)
        if (placement.contains(id) && placement.value(id) != QPair<int, int>{m_timeline->getClipPosition(id), m_timeline->getClipPlaytime(id)})
            affected.insert(id);
    result.insert(QStringLiteral("changed"), true);
    result.insert(QStringLiteral("newDuration"), m_timeline->duration());
    result.insert(QStringLiteral("removedClipIds"), idArray(sortedIds(before - after)));
    result.insert(QStringLiteral("newClipIds"), idArray(sortedIds(after - before)));
    result.insert(QStringLiteral("clipIdsAffected"), idArray(sortedIds(affected)));
    if (markers) result.insert(QStringLiteral("markersAdded"), markersAdded);
    result.insert(QStringLiteral("historySummary"), QJsonObject{{"source", source},
                                                                {"keep", keep},
                                                                {"ranges", int(ranges.size())},
                                                                {"removedFrames", removedFrames},
                                                                {"removedRanges", summaryRanges},
                                                                {"markersAdded", int(markersAdded.size())}});
    return result;
}

QJsonObject LiveBridge::startTranscription(const QJsonObject &command)
{
    const auto invalid = [](const QString &message) { return error(QStringLiteral("INVALID_COMMAND"), message); };
    if (!keys(command, {QStringLiteral("type"), QStringLiteral("binId"), QStringLiteral("engine"), QStringLiteral("model"), QStringLiteral("language")}))
        return invalid(QStringLiteral("transcribe takes binId, engine, model and language."));
    const QString binId = command.value(QStringLiteral("binId")).toString();
    const QString engine = command.value(QStringLiteral("engine")).toString(KdenliveSettings::speechEngine());
    if (!QRegularExpression(QStringLiteral("^[0-9]+$")).match(binId).hasMatch() || (engine != QLatin1String("whisper") && engine != QLatin1String("vosk")) ||
        (command.contains(QStringLiteral("model")) &&
         (!command.value(QStringLiteral("model")).isString() || command.value(QStringLiteral("model")).toString().trimmed().isEmpty())) ||
        (command.contains(QStringLiteral("language")) && !command.value(QStringLiteral("language")).isString()))
        return invalid(QStringLiteral("Expected binId, engine whisper or vosk, and string model/language."));
    std::shared_ptr<ProjectClip> clip;
    if (auto rejected = binClip(binId, clip); !rejected.isEmpty()) return rejected;
    if (!clip->hasAudio()) return error(QStringLiteral("NO_AUDIO"), QStringLiteral("Bin clip %1 has no audio stream.").arg(binId));
    if (!QFileInfo(clip->url()).isFile())
        return error(QStringLiteral("INCOMPATIBLE_MEDIA"), QStringLiteral("Transcription needs a clip backed by a local media file."));
    for (const auto &job : std::as_const(m_transcriptions))
        if (job->state == QLatin1String("running"))
            return error(QStringLiteral("TRANSCRIPTION_BUSY"), QStringLiteral("Wait for transcription of bin clip %1 to finish.").arg(job->binId));
    // Only report what is missing: installing Python packages or downloading models is the user's decision, made in Kdenlive's settings.
    std::unique_ptr<SpeechToText> stt;
    if (engine == QLatin1String("whisper"))
        stt = std::make_unique<SpeechToTextWhisper>();
    else
        stt = std::make_unique<SpeechToTextVosk>();
    const QStringList models = stt->getInstalledModels();
    QString model = command.value(QStringLiteral("model")).toString();
    if (model.isEmpty()) {
        const QString preferred = engine == QLatin1String("whisper") ? KdenliveSettings::whisperModel() : KdenliveSettings::vosk_text_model();
        model = models.contains(preferred) || models.isEmpty() ? preferred : models.constFirst();
    }
    const auto unavailable = [&](const QString &reason, const QString &message, const QStringList &missing = {}) {
        auto result = error(QStringLiteral("TRANSCRIPTION_UNAVAILABLE"), message);
        result.insert(QStringLiteral("reason"), reason);
        result.insert(QStringLiteral("engine"), engine);
        result.insert(QStringLiteral("model"), model);
        result.insert(QStringLiteral("installedModels"), QJsonArray::fromStringList(models));
        result.insert(QStringLiteral("missingDependencies"), QJsonArray::fromStringList(missing));
        result.insert(QStringLiteral("hint"),
                      QStringLiteral("Ask the user to set up %1 in Settings > Configure Kdenlive > Speech to Text (Python environment and a "
                                     "model). MCP never installs packages or downloads models.")
                          .arg(engine == QLatin1String("whisper") ? QStringLiteral("Whisper") : QStringLiteral("Vosk")));
        return result;
    };
    if (stt->speechScript().isEmpty())
        return unavailable(QStringLiteral("script_missing"), QStringLiteral("The %1 speech script is not installed with Kdenlive.").arg(engine));
    if (!stt->useSystemPython() && !stt->checkSetup(false))
        return unavailable(QStringLiteral("python_environment_missing"), QStringLiteral("The speech-to-text Python environment is not set up."));
    const QString python = stt->venvPythonExecs().python;
    if (python.isEmpty()) return unavailable(QStringLiteral("python_environment_missing"), QStringLiteral("No Python interpreter for speech to text."));
    stt->checkDependencies(true, false);
    const QStringList missing = stt->missingDependencies({engine == QLatin1String("whisper") ? QStringLiteral("openai-whisper") : QStringLiteral("vosk")});
    if (!missing.isEmpty())
        return unavailable(QStringLiteral("dependencies_missing"), QStringLiteral("Python packages are missing: %1.").arg(missing.join(QStringLiteral(", "))),
                           missing);
    if (models.isEmpty() || !models.contains(model))
        return unavailable(QStringLiteral("model_missing"), models.isEmpty() ? QStringLiteral("No %1 model is installed.").arg(engine)
                                                                             : QStringLiteral("Model %1 is not installed.").arg(model));
    auto job = std::make_shared<TranscriptionJob>();
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job->binId = binId;
    job->engine = engine;
    job->model = model;
    job->language = command.value(QStringLiteral("language")).toString();
    job->url = QFileInfo(clip->url()).absoluteFilePath();
    job->duration = clip->duration().seconds();
    job->document = m_document;
    job->started = QDateTime::currentMSecsSinceEpoch();
    if (m_request) {
        job->requestId = m_request->requestId;
        job->caller = m_request->caller;
    }
    // The same scripts and arguments as the Text-based edit panel's recognition of a file clip.
    QStringList args;
    if (engine == QLatin1String("whisper")) {
        QString language = job->language;
        if (KdenliveSettings::whisperDisableFP16()) language.append(QStringLiteral(" fp16=False"));
        args = {stt->speechScript(),
                QStringLiteral("--src=\"%1\"").arg(job->url),
                QStringLiteral("--model=%1").arg(model),
                QStringLiteral("--task=transcribe"),
                QStringLiteral("--language=%1").arg(language),
                QStringLiteral("--ffmpeg_path=%1").arg(KdenliveSettings::ffmpegpath())};
        if (!KdenliveSettings::whisperDevice().isEmpty()) args << QStringLiteral("--device=%1").arg(KdenliveSettings::whisperDevice());
    } else {
        args = {stt->speechScript(),
                QStringLiteral("--model_directory=%1").arg(stt->modelFolder()),
                QStringLiteral("--model=%1").arg(model),
                QStringLiteral("--src=\"%1\"").arg(job->url),
                QStringLiteral("--in_point=0"),
                QStringLiteral("--out_point=0"),
                QStringLiteral("--ffmpeg_path=%1").arg(KdenliveSettings::ffmpegpath())};
    }
    auto *process = new QProcess(this);
    job->process = process;
    connect(process, &QProcess::readyReadStandardOutput, this, [this, jobId, process] {
        auto job = m_transcriptions.value(jobId);
        if (!job) return;
        job->output.append(process->readAllStandardOutput());
        if (job->engine == QLatin1String("vosk") && job->duration > 0) {
            // Vosk prints results as it goes; the last word end tells how far it got.
            static const QRegularExpression end(QStringLiteral("\"end\"\\s*:\\s*([0-9.]+)"));
            double last = 0;
            for (auto it = end.globalMatch(QString::fromUtf8(job->output.right(4096))); it.hasNext();)
                last = it.next().captured(1).toDouble();
            job->progress = qBound(job->progress, int(100 * last / job->duration), 99);
        }
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, jobId, process] {
        auto job = m_transcriptions.value(jobId);
        if (!job) return;
        const QString text = QString::fromUtf8(process->readAllStandardError());
        job->log = QString(job->log + text).right(4000);
        // Whisper reports tqdm progress such as " 42%|####".
        static const QRegularExpression percent(QStringLiteral("(\\d{1,3})%\\|"));
        for (auto it = percent.globalMatch(text); it.hasNext();)
            job->progress = qBound(0, it.next().captured(1).toInt(), 99);
    });
    connect(process, &QProcess::finished, this, [this, jobId, process](int exitCode, QProcess::ExitStatus status) {
        process->deleteLater();
        auto job = m_transcriptions.value(jobId);
        if (!job) return;
        job->output.append(process->readAllStandardOutput());
        job->finished = QDateTime::currentMSecsSinceEpoch();
        if (status != QProcess::NormalExit || exitCode != 0) {
            job->state = QStringLiteral("failed");
            job->error = QStringLiteral("TRANSCRIPTION_FAILED: the %1 script exited with code %2. %3").arg(job->engine).arg(exitCode).arg(job->log.right(500));
            return;
        }
        // Whisper: "[start>end]" opens a segment, "[start>end]word" is a word. Vosk: JSON results with result[{conf, start, end, word}].
        QList<Word> words;
        const QString output = QString::fromUtf8(job->output);
        if (job->engine == QLatin1String("whisper")) {
            static const QRegularExpression line(QStringLiteral("^\\[([-0-9.e+]+)>([-0-9.e+]+)\\](.*)$"));
            int segment = -1;
            for (const auto &text : output.split(QLatin1Char('\n'))) {
                const auto match = line.match(text.trimmed());
                if (!match.hasMatch()) continue;
                if (match.captured(3).trimmed().isEmpty()) {
                    ++segment;
                    continue;
                }
                Word word;
                word.start = millis(match.captured(1).toDouble());
                word.end = millis(qMax(match.captured(1).toDouble(), match.captured(2).toDouble()));
                word.text = match.captured(3).simplified();
                word.segment = qMax(0, segment);
                words.append(word);
            }
        } else {
            int depth = 0, begin = -1, segment = 0;
            bool quoted = false;
            for (int i = 0; i < output.size(); ++i) {
                const QChar c = output.at(i);
                if (quoted) {
                    if (c == QLatin1Char('\\'))
                        ++i;
                    else if (c == QLatin1Char('"'))
                        quoted = false;
                    continue;
                }
                if (c == QLatin1Char('"'))
                    quoted = true;
                else if (c == QLatin1Char('{') && depth++ == 0)
                    begin = i;
                else if (c == QLatin1Char('}') && depth > 0 && --depth == 0 && begin >= 0) {
                    const auto object = QJsonDocument::fromJson(output.mid(begin, i - begin + 1).toUtf8()).object();
                    const auto result = object.value(QStringLiteral("result")).toArray();
                    for (const auto &item : result) {
                        const auto entry = item.toObject();
                        Word word;
                        word.start = millis(entry.value(QStringLiteral("start")).toDouble());
                        word.end = millis(qMax(word.start, entry.value(QStringLiteral("end")).toDouble()));
                        word.text = entry.value(QStringLiteral("word")).toString().simplified();
                        word.confidence = entry.contains(QStringLiteral("conf")) ? qBound(0.0, entry.value(QStringLiteral("conf")).toDouble(), 1.0) : -1;
                        word.segment = segment;
                        if (!word.text.isEmpty()) words.append(word);
                    }
                    if (!result.isEmpty()) ++segment;
                }
            }
        }
        std::stable_sort(words.begin(), words.end(), [](const Word &a, const Word &b) { return a.start < b.start; });
        job->wordCount = int(words.size());
        job->progress = 100;
        if (words.isEmpty()) {
            job->state = QStringLiteral("finished");
            job->error = QStringLiteral("No speech detected; nothing was stored.");
            return;
        }
        job->result.words = words;
        job->result.engine = job->engine;
        job->result.language = job->language;
        job->result.format = job->engine;
        finishTranscription(jobId);
    });
    m_transcriptions.insert(jobId, job);
    process->start(python, args);
    if (!process->waitForStarted(5000)) {
        m_transcriptions.remove(jobId);
        process->deleteLater();
        return error(QStringLiteral("TRANSCRIPTION_FAILED"), QStringLiteral("Cannot start %1.").arg(python));
    }
    return {{"ok", true}, {"jobId", jobId}, {"binId", binId}, {"engine", engine}, {"model", model}, {"language", job->language}, {"state", job->state}};
}

void LiveBridge::finishTranscription(const QString &jobId)
{
    auto job = m_transcriptions.value(jobId);
    if (!job || job->state != QLatin1String("running")) return;
    // Store between requests and outside modal dialogs, like any edit.
    if (m_executing || QApplication::activeModalWidget()) {
        QTimer::singleShot(500, this, [this, jobId] { finishTranscription(jobId); });
        return;
    }
    const auto fail = [&job](const QString &message) {
        job->state = QStringLiteral("failed");
        job->error = message;
    };
    if (!bind() || m_document.isNull() || m_document != job->document) return fail(QStringLiteral("The project was closed before transcription finished."));
    auto clip = pCore->projectItemModel()->getClipByBinID(job->binId);
    if (!clip) return fail(QStringLiteral("Bin clip %1 was removed before transcription finished.").arg(job->binId));
    // The stored transcript is an Undo entry of the transcribe request that started the job.
    QScopedValueRollback<bool> executing(m_executing, true);
    syncHistory();
    Request context{job->requestId, QStringLiteral("transcribe"), job->caller, m_revision, {}};
    QScopedValueRollback<Request *> current(m_request, &context);
    QJsonObject result = storeTranscript(m_document, clip, job->result, QStringLiteral("Transcribe clip"));
    if (result.isEmpty()) {
        result = {{"ok", true}, {"binId", job->binId}, {"historySummary", QJsonObject{{"wordCount", job->wordCount}, {"engine", job->engine}}}};
        contentChanged();
        job->state = QStringLiteral("finished");
        job->undoIndex = m_document->commandStack()->index();
    } else {
        fail(result.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString());
    }
    finishRequest(QJsonObject{{"type", "transcribe"}, {"binId", job->binId}, {"engine", job->engine}, {"model", job->model}}, result);
}

QJsonObject LiveBridge::transcribeStatus(const QJsonObject &arguments)
{
    if (!keys(arguments, {QStringLiteral("jobId")}) || (arguments.contains(QStringLiteral("jobId")) && !arguments.value(QStringLiteral("jobId")).isString()))
        return error(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("desktop_transcribe_status takes an optional jobId."));
    const QString wanted = arguments.value(QStringLiteral("jobId")).toString();
    if (!wanted.isEmpty() && !m_transcriptions.contains(wanted))
        return error(QStringLiteral("UNKNOWN_JOB"), QStringLiteral("No transcription job %1.").arg(wanted));
    QJsonArray jobs;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_transcriptions.cbegin(); it != m_transcriptions.cend(); ++it) {
        if (!wanted.isEmpty() && it.key() != wanted) continue;
        const auto &job = it.value();
        QJsonObject item{{"jobId", it.key()},         {"binId", job->binId},
                         {"engine", job->engine},     {"model", job->model},
                         {"language", job->language}, {"state", job->state},
                         {"progress", job->progress}, {"elapsedSeconds", int(((job->finished ? job->finished : now) - job->started) / 1000)}};
        if (job->state != QLatin1String("running")) item.insert(QStringLiteral("wordCount"), job->wordCount);
        if (job->undoIndex >= 0) item.insert(QStringLiteral("undoIndex"), job->undoIndex);
        if (!job->error.isEmpty()) item.insert(QStringLiteral("error"), job->error);
        jobs.append(item);
    }
    return {{"ok", true}, {"jobs", jobs}};
}
