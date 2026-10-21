// UTF-8 source, compiled with /utf-8 (layer/CMakeLists.txt).
#include "depthxr/osd_strings.h"

#include <array>
#include <cstddef>
#include <string>

#include "depthxr/settings.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace depthxr {
namespace {

constexpr std::size_t kLanguages = static_cast<std::size_t>(OsdLanguage::Count);

// In OsdLanguage order. Machine-translated like the app's catalogs; product
// names (app/src/i18n/glossary.json) stay English.
constexpr std::array<std::string_view, kLanguages> kCodes{"en", "zh-CN", "ru", "es-ES", "pt-BR", "de", "ja", "fr", "pl", "ko", "zh-TW"};

struct Entry {
    OsdText id;
    //           en, zh-CN, ru, es-ES, pt-BR, de, ja, fr, pl, ko, zh-TW
    std::array<const char*, kLanguages> text;
};

constexpr Entry kEntries[] = {
    {OsdText::AppFps, {"APP FPS", "应用 FPS", "FPS ПРИЛОЖЕНИЯ", "FPS DE LA APP", "FPS DO APP", "APP-FPS", "アプリ FPS", "FPS APPLI", "FPS APLIKACJI", "앱 FPS", "應用程式 FPS"}},
    {OsdText::AppFrameAvg, {"APP FRAME / AVG", "应用帧 / 平均", "КАДР ПРИЛ. / СРЕДН.", "FOTOGRAMA APP / MEDIA", "QUADRO DO APP / MÉDIA", "APP-FRAME / MITTEL", "アプリフレーム / 平均", "IMAGE APPLI / MOY.", "KLATKA APL. / ŚR.", "앱 프레임 / 평균", "應用程式影格 / 平均"}},
    {OsdText::AppFrameP95, {"APP FRAME / P95", "应用帧 / P95", "КАДР ПРИЛ. / P95", "FOTOGRAMA APP / P95", "QUADRO DO APP / P95", "APP-FRAME / P95", "アプリフレーム / P95", "IMAGE APPLI / P95", "KLATKA APL. / P95", "앱 프레임 / P95", "應用程式影格 / P95"}},
    {OsdText::MsAvg, {"{0} ms avg", "平均 {0} ms", "{0} ms сред.", "{0} ms media", "{0} ms média", "{0} ms Ø", "平均 {0} ms", "{0} ms moy.", "{0} ms śr.", "평균 {0} ms", "平均 {0} ms"}},
    {OsdText::FrameTimeGraph, {"FRAME TIME  /  LAST 120 FRAMES", "帧时间  /  最近 120 帧", "ВРЕМЯ КАДРА  /  ПОСЛЕДНИЕ 120 КАДРОВ", "TIEMPO DE FOTOGRAMA  /  ÚLTIMOS 120", "TEMPO DE QUADRO  /  ÚLTIMOS 120", "FRAMEZEIT  /  LETZTE 120 FRAMES", "フレーム時間  /  直近 120 フレーム", "TEMPS D’IMAGE  /  120 DERNIÈRES", "CZAS KLATKI  /  OSTATNIE 120", "프레임 시간  /  최근 120 프레임", "影格時間  /  最近 120 影格"}},
    {OsdText::Performance, {"PERFORMANCE", "性能", "ПРОИЗВОДИТЕЛЬНОСТЬ", "RENDIMIENTO", "DESEMPENHO", "LEISTUNG", "パフォーマンス", "PERFORMANCES", "WYDAJNOŚĆ", "성능", "效能"}},
    {OsdText::PerformanceCompact, {"PERF", "性能", "ПРОИЗВ.", "REND.", "DESEMP.", "LEIST.", "性能", "PERF.", "WYDAJN.", "성능", "效能"}},
    {OsdText::Turbo, {"TURBO", "TURBO", "TURBO", "TURBO", "TURBO", "TURBO", "TURBO", "TURBO", "TURBO", "TURBO", "TURBO"}},
    {OsdText::FrameGen, {"FRAME GEN", "FRAME GEN", "FRAME GEN", "FRAME GEN", "FRAME GEN", "FRAME GEN", "FRAME GEN", "FRAME GEN", "FRAME GEN", "FRAME GEN", "FRAME GEN"}},
    {OsdText::Pivot, {"PIVOT", "PIVOT", "PIVOT", "PIVOT", "PIVOT", "PIVOT", "PIVOT", "PIVOT", "PIVOT", "PIVOT", "PIVOT"}},
    {OsdText::Enhancements, {"ENHANCEMENTS", "增强功能", "УЛУЧШЕНИЯ", "MEJORAS", "MELHORIAS", "VERBESSERUNGEN", "拡張機能", "AMÉLIORATIONS", "ULEPSZENIA", "향상 기능", "增強功能"}},
    {OsdText::ExperimentalSuffix, {" / EXPERIMENTAL", " / 实验性", " / ЭКСПЕРИМЕНТ", " / EXPERIMENTAL", " / EXPERIMENTAL", " / EXPERIMENTELL", " / 実験的", " / EXPÉRIMENTAL", " / EKSPERYMENTALNE", " / 실험적", " / 實驗性"}},
    {OsdText::ExperimentalCompactSuffix, {" / EXP", " / 实验", " / ЭКСП.", " / EXP", " / EXP", " / EXP", " / 実験", " / EXP", " / EKSP.", " / 실험", " / 實驗"}},
    {OsdText::RestartPending, {"RESTART PENDING  /  {0}", "需要重启  /  {0}", "НУЖЕН ПЕРЕЗАПУСК  /  {0}", "REINICIO PENDIENTE  /  {0}", "REINÍCIO PENDENTE  /  {0}", "NEUSTART AUSSTEHEND  /  {0}", "再起動が必要  /  {0}", "REDÉMARRAGE REQUIS  /  {0}", "WYMAGANY RESTART  /  {0}", "재시작 필요  /  {0}", "需要重新啟動  /  {0}"}},
    {OsdText::Am, {"{0} AM", "上午 {0}", "{0} AM", "{0} a. m.", "{0} AM", "{0} AM", "午前 {0}", "{0} AM", "{0} AM", "오전 {0}", "上午 {0}"}},
    {OsdText::Pm, {"{0} PM", "下午 {0}", "{0} PM", "{0} p. m.", "{0} PM", "{0} PM", "午後 {0}", "{0} PM", "{0} PM", "오후 {0}", "下午 {0}"}},
    {OsdText::BadgeOn, {"ON", "开启", "ВКЛ", "ACTIVO", "ATIVO", "AN", "オン", "ACTIF", "WŁ.", "켜짐", "開啟"}},
    {OsdText::BadgeStandby, {"STBY", "待机", "ГОТОВ", "LISTO", "PRONTO", "BEREIT", "待機", "VEILLE", "GOTOWY", "대기", "待機"}},
    {OsdText::BadgeWait, {"WAIT", "等待", "ОЖИД.", "ESPERA", "AGUARDA", "WARTET", "待ち", "ATTENTE", "CZEKA", "기다림", "等待"}},
    {OsdText::BadgePaused, {"PAUSED", "已暂停", "ПАУЗА", "PAUSA", "PAUSADO", "PAUSIERT", "一時停止", "PAUSE", "PAUZA", "일시 정지", "已暫停"}},
    {OsdText::BadgeOff, {"OFF", "关闭", "ВЫКЛ", "INACTIVO", "DESL.", "AUS", "オフ", "INACTIF", "WYŁ.", "꺼짐", "關閉"}},
    {OsdText::BadgeUnavailable, {"N/A", "不可用", "Н/Д", "N/D", "N/D", "N/V", "利用不可", "N/D", "N/D", "사용 불가", "不可用"}},
    {OsdText::Disabled, {"Disabled", "已禁用", "Отключено", "Desactivado", "Desativado", "Deaktiviert", "無効", "Désactivé", "Wyłączone", "사용 안 함", "已停用"}},
    {OsdText::Off, {"Off", "关", "Выкл.", "Apagado", "Desligado", "Aus", "オフ", "Arrêt", "Wył.", "꺼짐", "關"}},
    {OsdText::Active, {"Active", "活动", "Активно", "Activo", "Ativo", "Aktiv", "アクティブ", "Actif", "Aktywne", "활성", "作用中"}},
    {OsdText::Suspended, {"Suspended", "已挂起", "Приостановлено", "Suspendido", "Suspenso", "Ausgesetzt", "中断中", "Suspendu", "Wstrzymane", "일시 중단", "已擱置"}},
    {OsdText::TurboPausedForFrameGen, {"Paused for Frame Generation", "因 Frame Generation 暂停", "Пауза ради Frame Generation", "En pausa por Frame Generation", "Pausado pelo Frame Generation", "Pausiert für Frame Generation", "Frame Generation のため一時停止", "En pause pour Frame Generation", "Wstrzymane dla Frame Generation", "Frame Generation으로 일시 정지", "因 Frame Generation 暫停"}},
    {OsdText::TurboEnabledOff, {"Enabled / Off", "已启用 / 关", "Включено / Выкл.", "Activado / Apagado", "Ativado / Desligado", "Aktiviert / Aus", "有効 / オフ", "Activé / Arrêt", "Włączone / Wył.", "사용 / 꺼짐", "已啟用 / 關"}},
    {OsdText::TurboRecoveryBlocked, {"Recovery blocked", "恢复已阻止", "Восстановление заблокировано", "Recuperación bloqueada", "Recuperação bloqueada", "Wiederherstellung blockiert", "回復がブロック中", "Récupération bloquée", "Odzyskiwanie zablokowane", "복구 차단됨", "復原已封鎖"}},
    {OsdText::TurboAsync, {"Async", "Async", "Async", "Async", "Async", "Async", "Async", "Async", "Async", "Async", "Async"}},
    {OsdText::TurboSequenced, {"Sequenced", "Sequenced", "Sequenced", "Sequenced", "Sequenced", "Sequenced", "Sequenced", "Sequenced", "Sequenced", "Sequenced", "Sequenced"}},
    {OsdText::TurboEnabledWaiting, {"Enabled / Waiting", "已启用 / 等待中", "Включено / Ожидание", "Activado / Esperando", "Ativado / Aguardando", "Aktiviert / Wartet", "有効 / 待機中", "Activé / En attente", "Włączone / Oczekiwanie", "사용 / 대기 중", "已啟用 / 等待中"}},
    {OsdText::TurboAnalyzingSuffix, {" / Analyzing", " / 分析中", " / Анализ", " / Analizando", " / Analisando", " / Analysiert", " / 分析中", " / Analyse", " / Analiza", " / 분석 중", " / 分析中"}},
    {OsdText::ModuleDepth, {"Depth", "深度", "Глубина", "Profundidad", "Profundidade", "Tiefe", "深度", "Profondeur", "Głębia", "깊이", "深度"}},
    {OsdText::ModulePivot, {"Pivot", "Pivot", "Pivot", "Pivot", "Pivot", "Pivot", "Pivot", "Pivot", "Pivot", "Pivot", "Pivot"}},
    {OsdText::ModuleTurbo, {"Turbo", "Turbo", "Turbo", "Turbo", "Turbo", "Turbo", "Turbo", "Turbo", "Turbo", "Turbo", "Turbo"}},
    {OsdText::ModuleFrameGen, {"Frame Gen", "Frame Gen", "Frame Gen", "Frame Gen", "Frame Gen", "Frame Gen", "Frame Gen", "Frame Gen", "Frame Gen", "Frame Gen", "Frame Gen"}},
    {OsdText::ModuleQuadviews, {"Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews"}},
    {OsdText::Motion, {"Motion: {0}", "运动：{0}", "Движение: {0}", "Movimiento: {0}", "Movimento: {0}", "Bewegung: {0}", "モーション: {0}", "Mouvement : {0}", "Ruch: {0}", "모션: {0}", "運動：{0}"}},
    {OsdText::MotionReleasing, {"Releasing", "释放中", "Отпускание", "Liberando", "Liberando", "Löst", "解除中", "Relâchement", "Zwalnianie", "해제 중", "釋放中"}},
    {OsdText::MotionContinuousShort, {"Cont", "连续", "Непр.", "Cont.", "Cont.", "Kont.", "連続", "Cont.", "Ciągł.", "연속", "連續"}},
    {OsdText::MotionSteppedShort, {"Step", "分步", "Шаг", "Paso", "Passo", "Stufe", "段階", "Pas", "Krok", "단계", "分段"}},
    {OsdText::MotionContinuous, {"Continuous", "连续", "Непрерывно", "Continuo", "Contínuo", "Kontinuierlich", "連続", "Continu", "Ciągły", "연속", "連續"}},
    {OsdText::MotionStepped, {"Stepped", "分步", "Пошагово", "Por pasos", "Em passos", "Gestuft", "段階", "Par paliers", "Krokowy", "단계", "分段"}},
    {OsdText::PlusPosition, {" + Position", " + 位置", " + Позиция", " + Posición", " + Posição", " + Position", " + 位置", " + Position", " + Pozycja", " + 위치", " + 位置"}},
    {OsdText::PlusNudge, {" + Nudge", " + 微调", " + Сдвиг", " + Ajuste", " + Ajuste", " + Versatz", " + 微調整", " + Décalage", " + Korekta", " + 미세 조정", " + 微調"}},
    {OsdText::SnapReturning, {"Snap: Returning", "快照：返回中", "Вид: возврат", "Vista: volviendo", "Vista: retornando", "Snap: Rückkehr", "スナップ: 復帰中", "Snap : retour", "Widok: powrót", "스냅: 복귀 중", "快照：返回中"}},
    {OsdText::SnapEntering, {"Snap: Entering", "快照：进入中", "Вид: вход", "Vista: entrando", "Vista: entrando", "Snap: Eintritt", "スナップ: 移行中", "Snap : entrée", "Widok: wejście", "스냅: 진입 중", "快照：進入中"}},
    {OsdText::SnapActive, {"Snap: Active", "快照：活动", "Вид: активен", "Vista: activa", "Vista: ativa", "Snap: Aktiv", "スナップ: アクティブ", "Snap : actif", "Widok: aktywny", "스냅: 활성", "快照：作用中"}},
    {OsdText::SnapOff, {"Snap: Off", "快照：关", "Вид: выкл.", "Vista: apagada", "Vista: desligada", "Snap: Aus", "スナップ: オフ", "Snap : arrêt", "Widok: wył.", "스냅: 꺼짐", "快照：關"}},
    {OsdText::SnapOffNudge, {"Snap: Off / Nudge", "快照：关 / 微调", "Вид: выкл. / сдвиг", "Vista: apagada / ajuste", "Vista: desligada / ajuste", "Snap: Aus / Versatz", "スナップ: オフ / 微調整", "Snap : arrêt / décalage", "Widok: wył. / korekta", "스냅: 꺼짐 / 미세 조정", "快照：關 / 微調"}},
    {OsdText::StackedSuffix, {" / Stacked", " / 叠加", " / Совмещено", " / Apilado", " / Empilhado", " / Gestapelt", " / 重ね", " / Cumulé", " / Nałożone", " / 중첩", " / 疊加"}},
    {OsdText::RestartTurboExperiments, {"Turbo experiments", "Turbo 实验", "Эксперименты Turbo", "Experimentos de Turbo", "Experimentos do Turbo", "Turbo-Experimente", "Turbo の実験", "Expériences Turbo", "Eksperymenty Turbo", "Turbo 실험", "Turbo 實驗"}},
    {OsdText::RestartQuadviews, {"Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews", "Quadviews"}},
    {OsdText::RestartFrameGeneration, {"Frame Generation", "Frame Generation", "Frame Generation", "Frame Generation", "Frame Generation", "Frame Generation", "Frame Generation", "Frame Generation", "Frame Generation", "Frame Generation", "Frame Generation"}},
    {OsdText::DiagnosticsDebugLog, {"DEBUG LOG", "调试日志", "ОТЛАДОЧНЫЙ ЖУРНАЛ", "REGISTRO DE DEPURACIÓN", "LOG DE DEPURAÇÃO", "DEBUG-LOG", "デバッグログ", "JOURNAL DE DÉBOGAGE", "DZIENNIK DEBUG", "디버그 로그", "偵錯記錄"}},
    {OsdText::DiagnosticsFlowView, {"FLOW VIEW", "光流视图", "ВИД ПОТОКА", "VISTA DE FLUJO", "VISTA DE FLUXO", "FLUSSANSICHT", "フロー表示", "VUE DU FLUX", "WIDOK PRZEPŁYWU", "흐름 보기", "光流檢視"}},
    {OsdText::DiagnosticsConfidenceView, {"CONFIDENCE VIEW", "置信度视图", "ВИД ДОСТОВЕРНОСТИ", "VISTA DE CONFIANZA", "VISTA DE CONFIANÇA", "KONFIDENZANSICHT", "信頼度表示", "VUE DE CONFIANCE", "WIDOK PEWNOŚCI", "신뢰도 보기", "信賴度檢視"}},
    {OsdText::DiagnosticsTintView, {"TINT VIEW", "着色视图", "ВИД ПОДСВЕТКИ", "VISTA DE TINTE", "VISTA DE TINTA", "TÖNUNGSANSICHT", "色付け表示", "VUE TEINTÉE", "WIDOK ZABARWIENIA", "색조 보기", "著色檢視"}},
    {OsdText::FpsTarget, {"{0} fps target", "目标 {0} fps", "цель {0} fps", "objetivo {0} fps", "meta de {0} fps", "Ziel {0} fps", "目標 {0} fps", "cible {0} fps", "cel {0} fps", "목표 {0} fps", "目標 {0} fps"}},
    {OsdText::CpuBound, {"CPU-BOUND", "CPU 瓶颈", "УПОР В CPU", "LÍMITE: CPU", "LIMITE: CPU", "CPU-LIMITIERT", "CPU ボトルネック", "LIMITÉ PAR CPU", "LIMIT: CPU", "CPU 병목", "CPU 瓶頸"}},
    {OsdText::GpuBound, {"GPU-BOUND", "GPU 瓶颈", "УПОР В GPU", "LÍMITE: GPU", "LIMITE: GPU", "GPU-LIMITIERT", "GPU ボトルネック", "LIMITÉ PAR GPU", "LIMIT: GPU", "GPU 병목", "GPU 瓶頸"}},
    {OsdText::CpuThread, {", thread {0}", "，线程 {0}", ", поток {0}", ", hilo {0}", ", thread {0}", ", Thread {0}", "、スレッド {0}", ", thread {0}", ", wątek {0}", ", 스레드 {0}", "，執行緒 {0}"}},
    {OsdText::CpuPipelined, {"pipelined frames", "流水线帧", "конвейерные кадры", "fotogramas en cola", "quadros em pipeline", "Pipeline-Frames", "パイプライン化フレーム", "images en pipeline", "klatki w potoku", "파이프라인 프레임", "管線化影格"}},
    {OsdText::CpuTurboPaces, {"Turbo paces the game", "Turbo 控制游戏节奏", "Turbo задаёт темп игры", "Turbo marca el ritmo", "Turbo dita o ritmo", "Turbo taktet das Spiel", "Turbo がペース制御中", "Turbo cadence le jeu", "Turbo nadaje tempo grze", "Turbo가 페이싱 중", "Turbo 控制遊戲節奏"}},
    {OsdText::CpuWaiting, {"waiting for frames", "等待帧", "ожидание кадров", "esperando fotogramas", "aguardando quadros", "wartet auf Frames", "フレーム待ち", "attente d’images", "oczekiwanie na klatki", "프레임 대기 중", "等待影格"}},
    {OsdText::GpuBusy, {"{0} busy", "占用 {0}", "загрузка {0}", "{0} ocupada", "{0} ocupada", "{0} ausgelastet", "使用率 {0}", "{0} occupé", "zajętość {0}", "사용률 {0}", "使用率 {0}"}},
    {OsdText::GpuGame, {", game {0}", "，游戏 {0}", ", игра {0}", ", juego {0}", ", jogo {0}", ", Spiel {0}", "、ゲーム {0}", ", jeu {0}", ", gra {0}", ", 게임 {0}", "，遊戲 {0}"}},
    {OsdText::GpuNoCounters, {"no GPU counters", "无 GPU 计数器", "нет счётчиков GPU", "sin contadores de GPU", "sem contadores da GPU", "keine GPU-Zähler", "GPU カウンターなし", "aucun compteur GPU", "brak liczników GPU", "GPU 카운터 없음", "無 GPU 計數器"}},
    {OsdText::GpuReading, {"reading counters", "正在读取计数器", "чтение счётчиков", "leyendo contadores", "lendo contadores", "liest Zähler", "カウンター読み取り中", "lecture des compteurs", "odczyt liczników", "카운터 읽는 중", "正在讀取計數器"}},
    {OsdText::VramOf, {"of {0}", "共 {0}", "из {0}", "de {0}", "de {0}", "von {0}", "/ {0}", "sur {0}", "z {0}", "/ {0}", "共 {0}"}},
    {OsdText::VramUnknown, {"unknown", "未知", "неизвестно", "desconocido", "desconhecido", "unbekannt", "不明", "inconnu", "nieznane", "알 수 없음", "未知"}},
};

constexpr bool EntriesInOrder() {
    if (std::size(kEntries) != static_cast<std::size_t>(OsdText::Count)) return false;
    for (std::size_t i = 0; i < std::size(kEntries); ++i) {
        if (static_cast<std::size_t>(kEntries[i].id) != i) return false;
        for (const char* text : kEntries[i].text) {
            if (!text || !*text) return false;
        }
    }
    return true;
}
static_assert(EntriesInOrder(), "kEntries must list every OsdText once, in order, with a text for every language");

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) return false;
    }
    return true;
}

#ifdef _WIN32
// The Windows display language as a tag ("de-DE"); empty when unavailable.
std::string WindowsDisplayLanguage() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (!LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0)) return {};
    std::string tag;
    for (const wchar_t* c = name; *c; ++c) tag.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    return tag;
}
#endif

OsdLanguage SystemLanguage() {
#ifdef _WIN32
    // The display language changes only with a sign-out.
    static const OsdLanguage language = [] {
        const OsdLanguage match = OsdLanguageFromTag(WindowsDisplayLanguage());
        return match == OsdLanguage::Count ? OsdLanguage::En : match;
    }();
    return language;
#else
    return OsdLanguage::En;
#endif
}

} // namespace

OsdLanguage OsdLanguageFromCode(std::string_view code) {
    for (std::size_t i = 0; i < kLanguages; ++i) {
        if (EqualsIgnoreCase(kCodes[i], code)) return static_cast<OsdLanguage>(i);
    }
    return OsdLanguage::Count;
}

std::string_view OsdLanguageCode(OsdLanguage language) {
    const auto index = static_cast<std::size_t>(language);
    return index < kLanguages ? kCodes[index] : kCodes[0];
}

OsdLanguage OsdLanguageFromTag(std::string_view tag) {
    std::string normalized(tag);
    for (char& c : normalized) c = c == '_' ? '-' : Lower(c);
    if (normalized.empty()) return OsdLanguage::Count;
    const std::string_view view(normalized);
    const std::string_view language = view.substr(0, view.find('-'));
    if (language == "zh") {
        for (const std::string_view traditional : {"-hant", "-tw", "-hk", "-mo"}) {
            const auto at = view.find(traditional);
            if (at != std::string_view::npos &&
                (at + traditional.size() == view.size() || view[at + traditional.size()] == '-')) {
                return OsdLanguage::ZhTw;
            }
        }
        return OsdLanguage::ZhCn;
    }
    if (const OsdLanguage exact = OsdLanguageFromCode(view); exact != OsdLanguage::Count) return exact;
    for (std::size_t i = 0; i < kLanguages; ++i) {
        if (EqualsIgnoreCase(kCodes[i].substr(0, kCodes[i].find('-')), language)) return static_cast<OsdLanguage>(i);
    }
    return OsdLanguage::Count;
}

OsdLanguage ResolveOsdLanguage(const OsdSettings& settings) {
    if (const OsdLanguage chosen = OsdLanguageFromCode(settings.language); chosen != OsdLanguage::Count) return chosen;
    if (const OsdLanguage app = OsdLanguageFromCode(settings.app_language); app != OsdLanguage::Count) return app;
    return SystemLanguage();
}

const char* OsdString(OsdLanguage language, OsdText text) {
    const auto index = static_cast<std::size_t>(text);
    if (index >= std::size(kEntries)) return "";
    const auto column = static_cast<std::size_t>(language);
    return kEntries[index].text[column < kLanguages ? column : 0];
}

std::string OsdFormat(OsdLanguage language, OsdText text, std::string_view value) {
    std::string out(OsdString(language, text));
    if (const auto at = out.find("{0}"); at != std::string::npos) out.replace(at, 3, value);
    return out;
}

const wchar_t* OsdFontFace(OsdLanguage language) {
    switch (language) {
    case OsdLanguage::ZhCn: return L"Microsoft YaHei UI";
    case OsdLanguage::ZhTw: return L"Microsoft JhengHei UI";
    case OsdLanguage::Ja: return L"Yu Gothic UI";
    case OsdLanguage::Ko: return L"Malgun Gothic";
    default: return L"Segoe UI";
    }
}

unsigned char OsdFontCharset(OsdLanguage language) {
#ifdef _WIN32
    // With the face missing, GDI's font mapper still picks one with the script.
    switch (language) {
    case OsdLanguage::ZhCn: return GB2312_CHARSET;
    case OsdLanguage::ZhTw: return CHINESEBIG5_CHARSET;
    case OsdLanguage::Ja: return SHIFTJIS_CHARSET;
    case OsdLanguage::Ko: return HANGUL_CHARSET;
    default: return DEFAULT_CHARSET;
    }
#else
    (void)language;
    return 1;
#endif
}

} // namespace depthxr
