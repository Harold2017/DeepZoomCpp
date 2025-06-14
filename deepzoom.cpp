#include "deepzoom.hpp"

#include <memory>
#include <numeric>
#include <cmath>

DeepZoomGenerator::DeepZoomGenerator(openslide_t* slide, int tile_size, int overlap, bool limit_bounds)
    : m_slide(slide), m_tile_size(tile_size), m_overlap(overlap), m_limit_bounds(limit_bounds)
{
    if (auto mpp_x = openslide_get_property_value(slide, OPENSLIDE_PROPERTY_NAME_MPP_X); mpp_x)
        if (auto mpp_y = openslide_get_property_value(slide, OPENSLIDE_PROPERTY_NAME_MPP_Y); mpp_y)
            m_mpp = (std::strtof(mpp_x, nullptr) + std::strtof(mpp_y, nullptr)) / 2.f;

    m_levels = openslide_get_level_count(m_slide);
    m_l_dimensions.reserve(m_levels);
    int64_t w = -1, h = -1;
    for (auto l = 0; l < m_levels; l++)
    {
        openslide_get_level_dimensions(m_slide, l, &w, &h);
        m_l_dimensions.push_back({w, h});
    }

    if (m_limit_bounds)
    {
        if (auto const* p = openslide_get_property_value(slide, OPENSLIDE_PROPERTY_NAME_BOUNDS_X); p)
            m_l0_offset.first = std::strtol(p, nullptr, 10);
        if (auto const* p = openslide_get_property_value(slide, OPENSLIDE_PROPERTY_NAME_BOUNDS_Y); p)
            m_l0_offset.second = std::strtol(p, nullptr, 10);

        auto l0_lim = m_l_dimensions[0];
        std::pair<double, double> size_scale{1., 1.};
        if (auto const* p = openslide_get_property_value(slide, OPENSLIDE_PROPERTY_NAME_BOUNDS_WIDTH); p)
            size_scale.first = std::strtol(p, nullptr, 10) / static_cast<double>(l0_lim.first);
        if (auto const* p = openslide_get_property_value(slide, OPENSLIDE_PROPERTY_NAME_BOUNDS_HEIGHT); p)
            size_scale.second = std::strtol(p, nullptr, 10) / static_cast<double>(l0_lim.second);

        for (auto& d : m_l_dimensions)
        {
            d.first = static_cast<int64_t>(std::ceil(d.first * size_scale.first));
            d.second = static_cast<int64_t>(std::ceil(d.second * size_scale.second));
        }
    }

    m_dzl_dimensions.push_back(m_l_dimensions[0]);
    while (m_dzl_dimensions.back().first > 1 || m_dzl_dimensions.back().second > 1)
        m_dzl_dimensions.push_back({std::max(int64_t{1}, (m_dzl_dimensions.back().first + 1) / 2),
                                    std::max(int64_t{1}, (m_dzl_dimensions.back().second + 1) / 2)});
    std::reverse(m_dzl_dimensions.begin(), m_dzl_dimensions.end());
    m_dz_levels = m_dzl_dimensions.size();

    m_t_dimensions.reserve(m_dz_levels);
    for (const auto& d : m_dzl_dimensions)
        m_t_dimensions.push_back({static_cast<int64_t>(std::ceil(static_cast<double>(d.first) / m_tile_size)),
                                  static_cast<int64_t>(std::ceil(static_cast<double>(d.second) / m_tile_size))});

    std::vector<double> level_0_dz_downsamples;
    level_0_dz_downsamples.reserve(m_dz_levels);
    m_preferred_slide_levels.reserve(m_dz_levels);
    for (auto l = 0; l < m_dz_levels; l++)
    {
        auto d = std::pow(2, (m_dz_levels - l - 1));
        level_0_dz_downsamples.push_back(d);
        m_preferred_slide_levels.push_back(openslide_get_best_level_for_downsample(m_slide, d));
    }

    m_level_downsamples.reserve(m_levels);
    for (auto l = 0; l < m_levels; l++)
        m_level_downsamples.push_back(openslide_get_level_downsample(m_slide, l));

    m_level_dz_downsamples.reserve(m_dz_levels);
    for (auto l = 0; l < m_dz_levels; l++)
        m_level_dz_downsamples.push_back(level_0_dz_downsamples[l] / m_level_downsamples[m_preferred_slide_levels[l]]);

    if (auto bg_color = openslide_get_property_value(m_slide, OPENSLIDE_PROPERTY_NAME_BACKGROUND_COLOR); bg_color)
        m_background_color = std::string("#") + bg_color;
}

int DeepZoomGenerator::level_count() const
{
    return m_dz_levels;
}

std::vector<std::pair<int64_t, int64_t>> DeepZoomGenerator::level_tiles() const
{
    return m_t_dimensions;
}

std::vector<std::pair<int64_t, int64_t>> DeepZoomGenerator::level_dimensions() const
{
    return m_dzl_dimensions;
}

int64_t DeepZoomGenerator::tile_count() const
{
    return std::reduce(m_t_dimensions.cbegin(), m_t_dimensions.cend(), int64_t{1},
                       [](auto s, auto const& d) { return s + d.first * d.second; });
}

std::tuple<int, int, std::vector<uint8_t>> DeepZoomGenerator::get_tile(int dz_level, int col, int row) const
{
    auto [info, z_size] = _get_tile_info(dz_level, col, row);
    auto const& [l0_location, slide_level, l_size] = info;
    auto const& [width, height] = l_size;
    auto const& [xx, yy] = l0_location;

    // https://openslide.org/docs/premultiplied-argb/
    auto buf = std::make_unique<uint32_t[]>(width * height);
    openslide_read_region(m_slide, buf.get(), xx, yy, slide_level, width, height);
    std::vector<uint8_t> data;
    data.reserve(width * height * 4);
    uint32_t p = 0;
    for (int64_t i = 0; i < width * height; i++)
    {
        p = buf[i];
        // according to the docs: OpenSlide emits samples as uint32_t, so on little-endian systems the output will need to be byte-swapped relative to the input.
        // i have to reverse the order to obtain the correct color
        data.push_back(p);       // b
        data.push_back(p >> 8);  // g
        data.push_back(p >> 16); // r
        data.push_back(p >> 24); // a
    }
    return std::make_tuple(width, height, std::move(data));
}

std::tuple<std::pair<int64_t, int64_t>, int, std::pair<int64_t, int64_t>> DeepZoomGenerator::get_tile_coordinates(
    int dz_level, int col, int row) const
{
    return std::get<0>(_get_tile_info(dz_level, col, row));
}

std::pair<int64_t, int64_t> DeepZoomGenerator::get_tile_dimensions(int dz_level, int col, int row) const
{
    return std::get<1>(_get_tile_info(dz_level, col, row));
}

std::string DeepZoomGenerator::get_dzi(std::string const& format) const
{
    auto const& [width, height] = m_l_dimensions[0];
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n \
<Image xmlns = \"http://schemas.microsoft.com/deepzoom/2008\"\n \
  Format=\"" +
           format + "\"\n \
  Overlap=\"" +
           std::to_string(m_overlap) + "\"\n \
  TileSize=\"" +
           std::to_string(m_tile_size) + "\"\n \
  >\n \
  <Size\n \
    Height=\"" +
           std::to_string(height) + "\"\n \
    Width=\"" +
           std::to_string(width) + "\"\n \
  />\n \
</Image>";
}

std::pair<std::tuple<std::pair<int64_t, int64_t>, // l0_location
                     int,                         // slide_level
                     std::pair<int64_t, int64_t>  // l_size
                     >,
          std::pair<int64_t, int64_t> // z_size
          >
DeepZoomGenerator::_get_tile_info(int dz_level, int col, int row) const
{
    // assert((dz_level >= 0 && dz_level < m_dz_levels), "invalid dz level");
    // assert((col >= 0 && col < m_t_dimensions[dz_level].first), "invalid dz col");
    // assert((row >= 0 && row < m_t_dimensions[dz_level].second), "invalid dz row");

    auto slide_level = m_preferred_slide_levels[dz_level];
    auto z_overlap_tl = std::make_pair(m_overlap * int(col != 0), m_overlap * int(row != 0));
    auto z_overlap_br = std::make_pair(m_overlap * int(col != m_t_dimensions[dz_level].first - 1),
                                       m_overlap * int(row != m_t_dimensions[dz_level].second - 1));
    auto z_location = std::make_pair(m_tile_size * col, m_tile_size * row);
    auto z_size = std::make_pair(std::min(m_tile_size, m_dzl_dimensions[dz_level].first - z_location.first) +
                                     z_overlap_tl.first + z_overlap_br.first,
                                 std::min(m_tile_size, m_dzl_dimensions[dz_level].second - z_location.second) +
                                     z_overlap_tl.second + z_overlap_br.second);
    auto l_dz_downsample = m_level_dz_downsamples[dz_level];
    auto l_location = std::make_pair(l_dz_downsample * (z_location.first - z_overlap_tl.first),
                                     l_dz_downsample * (z_location.second - z_overlap_tl.second));
    auto l_downsample = m_level_downsamples[slide_level];
    auto l0_location = std::make_pair(static_cast<int64_t>(l_downsample * l_location.first) + m_l0_offset.first,
                                      static_cast<int64_t>(l_downsample * l_location.second) + m_l0_offset.second);
    auto l_size = std::make_pair(
        std::min(static_cast<int64_t>(std::ceil(l_dz_downsample * z_size.first)),
                 m_l_dimensions[slide_level].first - static_cast<int64_t>(std::ceil(l_location.first))),
        std::min(static_cast<int64_t>(std::ceil(l_dz_downsample * z_size.second)),
                 m_l_dimensions[slide_level].second - static_cast<int64_t>(std::ceil(l_location.second))));
    return std::make_pair(std::make_tuple(l0_location, slide_level, l_size), z_size);
}