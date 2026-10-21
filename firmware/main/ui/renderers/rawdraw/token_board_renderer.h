/**
 * @file token_board_renderer.h
 * @brief AI balance and cost page, one provider per page. Data and the refresh logic live in board::BoardService
 *        (common/token_board.h); Providers() lists the pages.
 *
 * Layout (400x300): a header with the provider name and page dots; the top half is the totals (balance in big white on
 * black, today's and this hour's cost); the bottom half is the detail cards, at most 3 rows each, every row a yellow bar
 * whose length is its share of the card's top value. Bailian: yesterday and today by model (top 3). DeepSeek has no
 * usage API, so its card shows what the balance is made of (topped-up, granted).
 *
 * UP/DOWN = previous/next provider (no-op while there is only one), BOOT = refresh now. Entering the page refreshes
 * when the data is older than the refresh interval (done by the UI manager / SimUi, like Weather). The AccessKey and the
 * interval are set on the device web page (/tokenboard); without a key the page says so.
 */

#ifndef RAWDRAW_TOKEN_BOARD_RENDERER_H
#define RAWDRAW_TOKEN_BOARD_RENDERER_H

#include <string>
#include <vector>

#include "common/token_board.h"
#include "page_renderer.h"

namespace rawdraw {

class TokenBoardRenderer : public PageRenderer {
public:
    TokenBoardRenderer();
    ~TokenBoardRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    /** Address of the device web page shown in the hint, e.g. "192.168.1.20:8080" (no scheme, no path). */
    void SetAddress(const std::string& host) { host_ = host; }

    /** Index into board::Providers() of the page shown. */
    int ProviderIndex() const { return provider_; }

    /**
     * Call when board::BoardService reports a change. @return true if the panel should be redrawn: what is shown
     * differs from what the panel has (the start of a refresh alone does not redraw, the panel is slow).
     */
    bool NeedsRefreshAfterChange();

private:
    struct Row {
        std::string name;
        double value = 0;
    };
    struct Status {  // what the footer and the not-configured screen need, whichever provider is shown
        bool configured = false;
        bool loading = false;
        std::string error;
        time_t updated = 0;
        std::string signature;  // content that, when changed, needs a redraw
    };
    Status CurrentStatus() const;
    void RenderHeader(uint8_t* fb, int width, int height);
    void RenderHero(uint8_t* fb, int width, int height, bool has_balance, double balance, double today, double hour);
    void RenderBailian(uint8_t* fb, int width, int height);
    void RenderDeepSeek(uint8_t* fb, int width, int height);
    void RenderRowsCard(uint8_t* fb, int width, int height, const Rect& card, const char* title, Color chip,
                        const std::string& right_text, const std::vector<Row>& rows, const char* empty_text);

    std::string host_;
    std::string shown_sig_;
    int provider_ = 0;
    const lv_font_t* font_;
    const lv_font_t* title_font_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_TOKEN_BOARD_RENDERER_H
