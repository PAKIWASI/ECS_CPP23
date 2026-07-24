#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

constexpr uint16_t PAGE_SIZE          = 1024;
constexpr uint8_t  PAGE_VEC_INIT_SIZE = 1;

using page_no     = uint32_t;
using page_offset = uint16_t;

struct parr_addr
{
    page_no     page   = 0;
    page_offset offset = 0;
};

// Default-constructible is needed because pages are pre-allocated
// (std::array<T, PAGE_SIZE> default-constructs every slot), and
// move-constructible is needed for swap_remove/push_back semantics.
template <typename T>
concept PagedArrayElement =
    std::is_default_constructible_v<T> && std::is_move_constructible_v<T>;


template <PagedArrayElement T>
class PagedArray
{
  private:
    struct Page
    {
        uint16_t                  used = 0;
        std::array<T, PAGE_SIZE>  data{};
    };

    std::vector<std::unique_ptr<Page>> pages{};

  public:
    PagedArray()
    {
        pages.reserve(PAGE_VEC_INIT_SIZE);
        for (uint8_t i = 0; i < PAGE_VEC_INIT_SIZE; ++i) {
            new_page();
        }
    }

    // introspection

    [[nodiscard]] auto page_count() const noexcept -> size_t { return pages.size(); }

    [[nodiscard]] auto used_in(page_no p) const -> uint16_t
    {
        assert(p < pages.size() && "invalid page index");
        return pages[p]->used;
    }

    // element access

    [[nodiscard]] auto get(parr_addr addr) -> T&
    {
        assert(addr.page < pages.size() && "invalid page addr");
        assert(addr.offset < pages[addr.page]->used && "invalid page addr");
        return pages[addr.page]->data[addr.offset];
    }

    [[nodiscard]] auto get(parr_addr addr) const -> const T&
    {
        assert(addr.page < pages.size() && "invalid page addr");
        assert(addr.offset < pages[addr.page]->used && "invalid page addr");
        return pages[addr.page]->data[addr.offset];
    }

    // Forwarding reference + perfect forwarding lets callers pass either an
    // lvalue (copied) or rvalue (moved) without you writing two overloads,
    // and without the `const T` footgun that silently disabled moves.
    template <typename U>
    [[nodiscard]] auto push_back(U&& data) -> parr_addr
        requires std::constructible_from<T, U&&>
    {
        auto back_i = static_cast<page_no>(pages.size() - 1);

        if (pages[back_i]->used == PAGE_SIZE) {
            new_page();
            back_i = static_cast<page_no>(pages.size() - 1);
        }

        Page&       target = *pages[back_i];
        page_offset offset = target.used;

        target.data[offset] = std::forward<U>(data);
        ++target.used;

        return { .page=back_i, .offset=offset };
    }

    template <typename U>
    void put(parr_addr addr, U&& data)
        requires std::constructible_from<T, U&&>
    {
        assert(addr.page < pages.size() && "invalid page addr");
        assert(addr.offset < pages[addr.page]->used && "invalid page addr");
        pages[addr.page]->data[addr.offset] = std::forward<U>(data);
    }

    // Swap-removes within addr's own page, replacing the removed slot with
    // that page's current last element, then shrinks `used`.
    //
    // NOTE: this only guarantees "no gaps except possibly at the tail of
    // *this page*". It does not maintain a single, globally dense array
    // across all pages the way a classic ECS sparse-set does -- if you
    // swap_del from an early page while later pages are still full, that
    // early page now has a permanent hole that push_back will never reuse
    // (push_back only ever appends to the last page). If you need true
    // global density (e.g. so you can iterate contiguously with no
    // per-page bookkeeping), you'd instead track the global last element's
    // address, swap with *that*, and have the entity/sparse map on top of
    // this container update the moved element's address. That's a bigger
    // design change than a bug fix, so flagging it rather than assuming
    // it's what you want.
    void swap_del(parr_addr addr)
    {
        assert(addr.page < pages.size() && "invalid page addr");
        Page& p = *pages[addr.page];
        assert(addr.offset < p.used && "invalid page addr");

        uint16_t last = p.used - 1;
        if (addr.offset != last) {
            p.data[addr.offset] = std::move(p.data[last]);
        }
        p.data[last] = T{};   // drop the now-duplicate resource
        --p.used;
    }

    // Same as swap_del but hands the removed value back to the caller.
    [[nodiscard]] auto swap_remove(parr_addr addr) -> T
    {
        assert(addr.page < pages.size() && "invalid page addr");
        Page& p = *pages[addr.page];
        assert(addr.offset < p.used && "invalid page addr");

        uint16_t last = p.used - 1;
        T ret = std::move(p.data[addr.offset]);
        if (addr.offset != last) {
            p.data[addr.offset] = std::move(p.data[last]);
        }
        p.data[last] = T{};
        --p.used;
        return ret;
    }

  private:
    void new_page()
    {
        pages.emplace_back(std::make_unique<Page>());
    }
};
