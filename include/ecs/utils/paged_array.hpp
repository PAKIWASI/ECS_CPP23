#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

constexpr uint16_t PAGE_SIZE          = 1024;
constexpr uint8_t  PAGE_VEC_INIT_SIZE = 1;

using page_no      = uint32_t;
using page_offset  = uint16_t;
using parr_address = std::pair<page_no, page_offset>;



template <typename T> class PagedArray
{
  private:
    std::vector<std::unique_ptr<std::array<T, PAGE_SIZE>>> array{};
    std::vector<uint16_t>                                  page_offsets;

  public:
    PagedArray()
    {
        // TODO: modern c++ way of doing this?
        for (uint8_t i = 0; i < PAGE_VEC_INIT_SIZE; i++) {
            new_page();
        }
    }


    [[nodiscard]] auto push_back(const T data) -> parr_address
    {
        size_t   back_i = array.size() - 1;
        uint16_t offset = page_offsets[back_i]++;

        if (offset == PAGE_SIZE) {
            new_page();
            back_i++;
            offset = 0;
        }

        array[back_i][offset] = std::move(data);

        return {back_i, offset};
    }

    void swap_del(parr_address addr)
    {
        uint16_t used = page_offsets[addr.first];

        assert(addr.first < array.size() && "invalid page addr");
        assert(addr.second < used && "invalid page addr");

        // TODO: is the replaced elm deleted properly?
        array[addr.first][addr.second] = std::move(array[addr.first][used - 1]);
    }

    // move value back to caller
    [[nodiscard]] auto swap_remove(parr_address addr) -> T
    {
        uint16_t used = page_offsets[addr.first];

        assert(addr.first < array.size() && "invalid page addr");
        assert(addr.second < used && "invalid page addr");

        // TODO: any better way to do this
        T ret = std::move(array[addr.first][addr.second]);
        array[addr.first][addr.second] = std::move(array[addr.first][used - 1]);
        return std::move(ret);
    }

  private:

    void new_page()
    {
        array.emplace_back(std::make_unique<std::array<T, PAGE_SIZE>>());
        page_offsets.emplace_back(0);
    }


};


