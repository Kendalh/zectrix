#include "token_board_renderer.h"

#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "rawdraw/components/footer_bar.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {

constexpr int kMargin = 8;
constexpr int kGap = 8;
constexpr int kHeaderTop = Style::kStatusBarHeight + 4;
constexpr int kHeaderH = 22;
constexpr int kHeroTop = kHeaderTop + kHeaderH + 5;
constexpr int kHeroH = 100;                       // the totals take the top half of the page
constexpr int kCardTop = kHeroTop + kHeroH + kGap;
constexpr int kRadius = 10;
constexpr int kRowH = 24;
constexpr int kMaxRows = 3;                       // detail cards show the top 3 only

size_t Utf8Len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    return 4;
}

// Cut `text` to at most max_w, ending in "…" when shortened.
std::string FitOneLine(const std::string& text, const lv_font_t* font, int max_w) {
    if (MeasureTextWidth(text.c_str(), font) <= max_w) return text;
    const int ellipsis_w = MeasureTextWidth("…", font);
    std::string out;
    int w = 0;
    for (size_t i = 0; i < text.size();) {
        const size_t len = std::min(Utf8Len(static_cast<unsigned char>(text[i])), text.size() - i);
        const std::string ch = text.substr(i, len);
        w += MeasureTextWidth(ch.c_str(), font);
        if (w + ellipsis_w > max_w) break;
        out += ch;
        i += len;
    }
    return out + "…";
}

void DrawLeft(uint8_t* fb, int width, int x, int cy, const std::string& text, const lv_font_t* font, Color color) {
    DrawText(fb, width, x, InkCenteredTextTopY(font, text.c_str(), cy, 0), text.c_str(), font, color);
}

void DrawRight(uint8_t* fb, int width, int right, int cy, const std::string& text, const lv_font_t* font, Color color) {
    DrawLeft(fb, width, right - MeasureTextWidth(text.c_str(), font), cy, text, font, color);
}

// The only fonts are 16 px, so the hero number is drawn into a scratch buffer and magnified `scale` times. The magnified
// ink (not the font's line box) is centered on `cy`.
void DrawTextScaled(uint8_t* fb, int width, int x, int cy, const std::string& text, const lv_font_t* font, Color color,
                    int scale) {
    // Size the scratch from the real ink bounds, not font->line_height: that field is 0 for this font on the device,
    // which gave a 2-row scratch and a blank balance. DrawText(y) puts ink at y+bounds.top..y+bounds.bottom.
    const TextInkBounds bounds = MeasureTextInkBounds(font, text.c_str());
    if (!bounds.valid) return;
    const int tw = MeasureTextWidth(text.c_str(), font) + 2;
    const int th = bounds.height + 2;
    std::vector<uint8_t> scratch(static_cast<size_t>((tw * 2 + 7) >> 3) * th, 0x55);  // 0x55 = all WHITE
    DrawText(scratch.data(), tw, 0, 1 - bounds.top, text.c_str(), font, BLACK);
    int ink_top = th, ink_bottom = -1;
    for (int sy = 0; sy < th; ++sy) {
        for (int sx = 0; sx < tw; ++sx) {
            if (get_pixel(scratch.data(), tw, sx, sy) == BLACK) {
                ink_top = std::min(ink_top, sy);
                ink_bottom = std::max(ink_bottom, sy);
            }
        }
    }
    if (ink_bottom < 0) return;
    const int top = cy - (ink_top + ink_bottom + 1) * scale / 2;
    for (int sy = ink_top; sy <= ink_bottom; ++sy) {
        for (int sx = 0; sx < tw; ++sx) {
            if (get_pixel(scratch.data(), tw, sx, sy) == BLACK) {
                FillRect(fb, width, {x + sx * scale, top + sy * scale, scale, scale}, color);
            }
        }
    }
}

int ScaledWidth(const std::string& text, const lv_font_t* font, int scale) {
    return (MeasureTextWidth(text.c_str(), font) + 2) * scale;
}

std::string Yuan(double amount) { return "¥" + board::FormatYuan(amount); }

}  // namespace

TokenBoardRenderer::TokenBoardRenderer()
    : font_(&SourceHanSansSC_Regular_slim), title_font_(&SourceHanSansSC_Medium_slim) {}

TokenBoardRenderer::~TokenBoardRenderer() = default;

void TokenBoardRenderer::Init(int, int) {
    provider_ = 0;
    needs_full_refresh_ = true;
}

namespace {
void RefreshProvider(int provider, bool force) {
    if (provider == 0) board::BoardService::Get().Refresh(force);
    else board::DeepSeekService::Get().Refresh(force);
}

std::string SignatureOf(const board::DayBill& bill) {
    std::string sig = bill.ok ? "|ok" : "|--";
    for (const auto& m : bill.models) sig += m.model + std::to_string(m.cost);
    return sig;
}
}  // namespace

bool TokenBoardRenderer::HandleInput(const ButtonEvent& event) {
    const int count = static_cast<int>(board::Providers().size());
    switch (event.type) {
        case ButtonEvent::kBootClick:
            RefreshProvider(provider_, true);
            return true;
        case ButtonEvent::kUpClick:
        case ButtonEvent::kDownClick:
            if (count < 2) return false;  // one provider: nothing to page to, no panel refresh
            provider_ = (provider_ + (event.type == ButtonEvent::kDownClick ? 1 : count - 1)) % count;
            needs_full_refresh_ = true;
            RefreshProvider(provider_, false);  // stale data of the newly shown provider is fetched now
            return true;
        default:
            return false;
    }
}

TokenBoardRenderer::Status TokenBoardRenderer::CurrentStatus() const {
    Status st;
    std::string sig = std::to_string(provider_);
    if (provider_ == 0) {
        const board::Snapshot s = board::BoardService::Get().GetSnapshot();
        st = {s.configured, s.loading, s.error, s.updated, ""};
        sig += std::string(s.configured ? "c" : "n") + "|" + s.error + "|" + std::to_string(s.updated) + "|" +
               std::to_string(s.balance) + "|" + std::to_string(s.today_cost) + "|" + std::to_string(s.hour_cost) +
               SignatureOf(s.yesterday) + SignatureOf(s.today);
    } else {
        const board::DeepSeekSnapshot s = board::DeepSeekService::Get().GetSnapshot();
        st = {s.configured, s.loading, s.error, s.updated, ""};
        sig += std::string(s.configured ? "c" : "n") + "|" + s.error + "|" + std::to_string(s.updated) + "|" +
               std::to_string(s.balance.primary.total) + "|" + std::to_string(s.today_cost) + "|" +
               std::to_string(s.hour_cost) + (s.balance.available ? "|a" : "|u");
    }
    st.signature = sig;
    return st;
}

bool TokenBoardRenderer::NeedsRefreshAfterChange() { return CurrentStatus().signature != shown_sig_; }

void TokenBoardRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const Status st = CurrentStatus();
    shown_sig_ = st.signature;
    FillRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, WHITE);
    RenderHeader(fb, width, height);

    const int providers = static_cast<int>(board::Providers().size());
    FooterBar footer;
    footer.SetBounds(width, height);

    if (!st.configured) {
        const char* msg = provider_ == 0 ? "尚未设置 AccessKey" : "尚未设置 API Key";
        const std::string hint = "在浏览器打开 " + (host_.empty() ? std::string("设备地址") : "http://" + host_) + "/tokenboard";
        const int cy = (kHeroTop + height - Style::kFooterBarHeight) / 2;
        DrawRoundRect(fb, width, height, {kMargin, kHeroTop, width - 2 * kMargin, height - Style::kFooterBarHeight - kHeroTop - kGap},
                      kRadius, WHITE, BLACK, 2);
        DrawText(fb, width, (width - MeasureTextWidth(msg, title_font_)) / 2, InkCenteredTextTopY(title_font_, msg, cy - 12, 0),
                 msg, title_font_, BLACK);
        DrawText(fb, width, std::max(4, (width - MeasureTextWidth(hint.c_str(), font_)) / 2),
                 InkCenteredTextTopY(font_, hint.c_str(), cy + 18, 0), hint.c_str(), font_, RED);
        footer.SetText(nullptr, providers > 1 ? "UP/DN切换" : nullptr, nullptr);
        footer.Draw(fb, width, height);
        return;
    }

    if (provider_ == 0) RenderBailian(fb, width, height);
    else RenderDeepSeek(fb, width, height);

    std::string status;
    if (st.loading) {
        status = "刷新中…";
    } else if (!st.error.empty()) {
        status = FitOneLine("失败：" + st.error, font_, width / 2 + 40);
    } else if (st.updated) {
        const board::DateParts t = board::BeijingParts(st.updated);
        char buf[24];
        snprintf(buf, sizeof(buf), "更新 %02d:%02d", t.hour, static_cast<int>((st.updated % 3600) / 60));
        status = buf;
    }
    footer.SetText("BOOT刷新", providers > 1 ? "UP/DN切换" : nullptr, status.empty() ? nullptr : status.c_str());
    footer.Draw(fb, width, height);
}

// Provider name as a black pill on the left, page dots (filled = current) on the right.
void TokenBoardRenderer::RenderHeader(uint8_t* fb, int width, int height) {
    const auto& providers = board::Providers();
    const char* name = providers[provider_].name;
    const int pill_w = MeasureTextWidth(name, title_font_) + 24;
    DrawRoundRect(fb, width, height, {kMargin, kHeaderTop, pill_w, kHeaderH}, kHeaderH / 2, BLACK, BLACK, 0);
    DrawLeft(fb, width, kMargin + 12, kHeaderTop + kHeaderH / 2, name, title_font_, WHITE);
    DrawLeft(fb, width, kMargin + pill_w + 10, kHeaderTop + kHeaderH / 2, "AI 余额和费用", font_, BLACK);

    constexpr int kDot = 10;
    const int n = static_cast<int>(providers.size());
    int x = width - kMargin - n * kDot - (n - 1) * 6;
    for (int i = 0; i < n; ++i, x += kDot + 6) {
        DrawRoundRect(fb, width, height, {x, kHeaderTop + (kHeaderH - kDot) / 2, kDot, kDot}, kDot / 2,
                      i == provider_ ? BLACK : WHITE, BLACK, 2);
    }
}

// Top half: balance on black in the biggest text that fits, today and this hour stacked on the right.
// Today/hour are balance-based estimates (see token_board.h).
void TokenBoardRenderer::RenderHero(uint8_t* fb, int width, int height, bool has_balance, double balance, double today,
                                    double hour) {
    const int hero_w = 208;
    const Rect hero = {kMargin, kHeroTop, hero_w, kHeroH};
    DrawRoundRect(fb, width, height, hero, kRadius, BLACK, BLACK, 0);
    DrawLeft(fb, width, hero.x + 16, hero.y + 18, "余额", font_, WHITE);
    FillRect(fb, width, {hero.x + 16, hero.y + 32, hero_w - 32, 1}, WHITE);
    const std::string text = has_balance ? Yuan(balance) : "—";
    ESP_LOGI("TokenBoard", "hero: has_balance=%d balance=%f today=%f hour=%f text='%s'", has_balance, balance, today, hour,
             text.c_str());
    const int scale = ScaledWidth(text, title_font_, 2) <= hero_w - 32 ? 2 : 1;
    DrawTextScaled(fb, width, hero.x + 16, hero.y + 66, text, title_font_, WHITE, scale);

    const int side_x = hero.x + hero_w + kGap;
    const int side_w = width - kMargin - side_x;
    const int side_h = (kHeroH - kGap) / 2;
    struct Small { const char* label; double value; Color fill; Color ink; };
    const Small smalls[2] = {{"今日", today, WHITE, RED}, {"本小时", hour, YELLOW, BLACK}};
    for (int i = 0; i < 2; ++i) {
        const Rect r = {side_x, kHeroTop + i * (side_h + kGap), side_w, side_h};
        DrawRoundRect(fb, width, height, r, kRadius, smalls[i].fill, BLACK, 2);
        DrawLeft(fb, width, r.x + 14, r.y + side_h / 2, smalls[i].label, font_, BLACK);
        DrawRight(fb, width, r.x + side_w - 14, r.y + side_h / 2, has_balance ? Yuan(smalls[i].value) : "—", title_font_,
                  smalls[i].ink);
    }
}

void TokenBoardRenderer::RenderBailian(uint8_t* fb, int width, int height) {
    const board::Snapshot snap = board::BoardService::Get().GetSnapshot();
    RenderHero(fb, width, height, snap.has_balance, snap.balance, snap.today_cost, snap.hour_cost);

    const int card_h = height - Style::kFooterBarHeight - kGap / 2 - kCardTop;
    const int card_w = (width - 2 * kMargin - kGap) / 2;
    const struct { const char* title; Color chip; const board::DayBill* bill; bool pending; } days[2] = {
        {"昨日", BLACK, &snap.yesterday, snap.loading && !snap.yesterday.ok},
        {"今日", RED, &snap.today, snap.loading && !snap.today.ok}};
    for (int i = 0; i < 2; ++i) {
        std::vector<Row> rows;
        for (size_t m = 0; m < days[i].bill->models.size() && m < static_cast<size_t>(kMaxRows); ++m)
            rows.push_back({days[i].bill->models[m].model, days[i].bill->models[m].cost});
        RenderRowsCard(fb, width, height, {kMargin + i * (card_w + kGap), kCardTop, card_w, card_h}, days[i].title, days[i].chip,
                       days[i].bill->ok ? Yuan(days[i].bill->total) : "", rows,
                       days[i].bill->ok ? "暂无账单" : (days[i].pending ? "加载中…" : "—"));
    }
}

// DeepSeek has no usage API an API key can call, so the detail card shows what the balance consists of.
void TokenBoardRenderer::RenderDeepSeek(uint8_t* fb, int width, int height) {
    const board::DeepSeekSnapshot snap = board::DeepSeekService::Get().GetSnapshot();
    RenderHero(fb, width, height, snap.has_balance, snap.balance.primary.total, snap.today_cost, snap.hour_cost);

    std::vector<Row> rows;
    if (snap.has_balance) {
        rows.push_back({"充值余额", snap.balance.primary.topped_up});
        rows.push_back({"赠送余额", snap.balance.primary.granted});
        for (const auto& line : snap.balance.lines)
            if (line.currency != snap.balance.primary.currency) rows.push_back({line.currency + " 余额", line.total});
    }
    const int card_h = height - Style::kFooterBarHeight - kGap / 2 - kCardTop;
    RenderRowsCard(fb, width, height, {kMargin, kCardTop, width - 2 * kMargin, card_h}, "余额构成",
                   snap.has_balance && !snap.balance.available ? RED : BLACK,
                   snap.has_balance ? (snap.balance.available ? snap.balance.primary.currency + " 可用" : "余额不足") : "", rows,
                   snap.loading ? "加载中…" : "—");
}

// A card with a title chip and a right-aligned text, then at most kMaxRows rows, each with a yellow bar behind it whose
// length is the row's share of the largest value.
void TokenBoardRenderer::RenderRowsCard(uint8_t* fb, int width, int height, const Rect& card, const char* title, Color chip,
                                        const std::string& right_text, const std::vector<Row>& rows, const char* empty_text) {
    DrawRoundRect(fb, width, height, card, kRadius, WHITE, BLACK, 2);
    const int head_cy = card.y + 17;
    const int chip_w = MeasureTextWidth(title, font_) + 18;
    DrawRoundRect(fb, width, height, {card.x + 8, head_cy - 9, chip_w, 18}, 9, chip, chip, 0);
    DrawLeft(fb, width, card.x + 17, head_cy, title, font_, WHITE);
    if (!right_text.empty()) DrawRight(fb, width, card.x + card.w - 12, head_cy, right_text, title_font_, BLACK);

    const int inner_x = card.x + 8;
    const int inner_w = card.w - 16;
    int row_y = card.y + 31;
    if (rows.empty()) {
        DrawLeft(fb, width, inner_x + 4, row_y + kRowH / 2, empty_text, font_, BLACK);
        return;
    }
    double top = 0;
    for (const auto& r : rows) top = std::max(top, r.value);
    if (top <= 0) top = 1;
    for (size_t i = 0; i < rows.size() && i < static_cast<size_t>(kMaxRows); ++i, row_y += kRowH) {
        const int bar_w = std::max(8, static_cast<int>(inner_w * std::min(1.0, rows[i].value / top)));
        DrawRoundRect(fb, width, height, {inner_x, row_y + 2, bar_w, kRowH - 4}, 6, YELLOW, YELLOW, 0);
        const std::string amount = board::FormatYuan(rows[i].value);
        const int amount_w = MeasureTextWidth(amount.c_str(), font_);
        DrawLeft(fb, width, inner_x + 5, row_y + kRowH / 2, FitOneLine(rows[i].name, font_, inner_w - amount_w - 16), font_, BLACK);
        DrawRight(fb, width, inner_x + inner_w - 4, row_y + kRowH / 2, amount, font_, BLACK);
    }
}

}  // namespace rawdraw
