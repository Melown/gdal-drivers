/*
 * @file solid.cpp
 */

#include <cstdlib>
#include <algorithm>
#include <vector>
#include <iterator>
#include <fstream>
#include <iomanip>

#include "dbglog/dbglog.hpp"

#include "utility/raise.hpp"
#include "utility/multivalue.hpp"
#include "utility/streams.hpp"
#include "geo/gdal.hpp"
#include "geo/po.hpp"

#include "./solid.hpp"

namespace po = boost::program_options;

namespace gdal_drivers {

/**
 * @brief BorderedAreaRasterBand
 */
class SolidDataset::RasterBand : public ::GDALRasterBand {
public:
    RasterBand(SolidDataset *dset, const Config::Band &band
               , const std::vector<math::Size2> &overviews);

    virtual ~RasterBand() {
        for (auto &ovr : ovrBands_) { delete ovr; }
    }

    virtual CPLErr IReadBlock(int blockCol, int blockRow, void *image);

    virtual GDALColorInterp GetColorInterpretation() {
        return colorInterpretation_;
    }

    virtual int GetOverviewCount() { return overviews_.size(); }

    virtual GDALRasterBand* GetOverview(int index) {
        if (index >= int(ovrBands_.size())) { return nullptr; }
        auto &ovr(ovrBands_[index]);
        if (!ovr) {
            ovr = new OvrBand(this, overviews_[index]);
        }
        return ovr;
    }

private:
    class OvrBand : public ::GDALRasterBand {
    public:
        OvrBand(RasterBand *owner, const math::Size2 &size)
            : owner_(owner)
        {
            poDS = owner->poDS;
            nBand = owner->nBand;
            nBlockXSize = owner->nBlockXSize;
            nBlockYSize = owner->nBlockYSize;
            eDataType = owner->eDataType;

            nRasterXSize = size.width;
            nRasterYSize = size.height;
        }

        virtual CPLErr IReadBlock(int blockCol, int blockRow, void *image) {
            return owner_->IReadBlock(blockCol, blockRow, image);
        }

        virtual GDALColorInterp GetColorInterpretation() {
            return owner_->GetColorInterpretation();
        }

    private:
        RasterBand *owner_;
    };

    friend class OvrBand;

    template <typename T>
    void createBlock(const T &value, std::size_t count)
    {
        auto *block(new T[count]);
        std::fill_n(block, count, value);
        block_ = std::shared_ptr<T>(block, [](T *block) { delete [] block; });
        blockSize_ = count * sizeof(T);
    }

    /** Block of pregenerated data.
     */
    std::shared_ptr<void> block_;

    /** Size of block of pregenerated data in bytes.
     */
    std::size_t blockSize_;

    ::GDALColorInterp colorInterpretation_;

    std::vector<math::Size2> overviews_;
    std::vector< ::GDALRasterBand*> ovrBands_;
};

namespace {

struct GeoTransformWrapper {
    geo::GeoTransform value;
};

template<typename CharT, typename Traits>
inline std::basic_istream<CharT, Traits>&
operator>>(std::basic_istream<CharT, Traits> &is, GeoTransformWrapper &g)
{
    auto &v(g.value);
    auto comma(utility::expect<CharT>(','));
    return is >> v[0] >> comma >> v[1] >> comma >> v[2]
              >> comma >> v[3] >> comma >> v[4] >> comma >> v[5];
}

template<typename CharT, typename Traits>
inline std::basic_ostream<CharT, Traits>&
operator<<(std::basic_ostream<CharT, Traits> &os, const GeoTransformWrapper &g)
{
    const auto &v(g.value);
    return os << v[0] << ',' << v[1] << ',' << v[2]
              << ',' << v[3] << ',' << v[4] << ',' << v[5];
}

} // namespace

GDALDataset* SolidDataset::Open(GDALOpenInfo *openInfo)
{
    ::CPLErrorReset();

    po::options_description config("solid color GDAL driver");
    po::variables_map vm;
    Config cfg;

    config.add_options()
        ("solid.srs", po::value(&cfg.srs)->required()
         , "SRS definition. Use [WKT], +proj or EPSG:num.")
        ("solid.size", po::value(&cfg.size)->required()
         , "Size of dataset (WxH).")
        ("solid.extents", po::value<math::Extents2>()
         , "Geo extents of dataset (ulx,uly:urx,ury).")
        ("solid.geoTransform", po::value<GeoTransformWrapper>()
         , "Geo transform matrix (m00, m01, m02, m10, m11, m12).")
        ("solid.tileSize", po::value(&cfg.tileSize)
         ->default_value(math::Size2(256, 256))->required()
         , "Tile size.")
        ;

    using utility::multi_value;

    config.add_options()
        ("band.value", multi_value<decltype(Config::Band::value)>()
         , "Value to return.")
        ("band.dataType", multi_value<decltype(Config::Band::dataType)>()
         , "Data type.")
        ("band.colorInterpretation"
         , multi_value<decltype(Config::Band::colorInterpretation)>()
         , "Color interpretation.")
        ;

    po::basic_parsed_options<char> parsed(&config);

    // try to parse file -> cannot parse -> probably not a solid file format
    try {
        std::ifstream f;
        f.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        f.open(openInfo->pszFilename);
        f.exceptions(std::ifstream::badbit);
        parsed = (po::parse_config_file(f, config));
        if (parsed.options.empty()) {
            // structure valid but nothing read -> not a solid file
            return nullptr;
        }
    } catch (...) { return nullptr; }

    // no updates
    if (openInfo->eAccess == GA_Update) {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "The Quadtree Solid driver does not support update "
                 "access to existing datasets.\n");
        return nullptr;
    }

    // initialize dataset
    try {
        po::store(parsed, vm);
        po::notify(vm);

        bool hasExtents(vm.count("solid.extents"));
        bool hasGeoTransform(vm.count("solid.geoTransform"));

        if (hasExtents && hasGeoTransform) {
            CPLError(CE_Failure, CPLE_IllegalArg
                     , "SolidDataset initialization failure:"
                     " both extents and geoTransform are set.\n");
            return nullptr;
        }
        if (!hasExtents && !hasGeoTransform) {
            CPLError(CE_Failure, CPLE_IllegalArg
                     , "SolidDataset initialization failure:"
                     " both extents and geoTransform are unset.\n");
            return nullptr;
        }

        if (hasExtents) {
            cfg.extents(vm["solid.extents"].as<math::Extents2>());
        } else {
            cfg.geoTransform
                (vm["solid.geoTransform"].as<GeoTransformWrapper>().value);
        }

        // process bands
        auto &bands(cfg.bands);
        bands.resize(vm["band.value"].as<std::vector<double> >().size());
        using utility::process_multi_value;
        process_multi_value(vm, "band.value"
                            , bands, &Config::Band::value);
        process_multi_value(vm, "band.dataType"
                            , bands, &Config::Band::dataType);
        process_multi_value(vm, "band.colorInterpretation"
                            , bands, &Config::Band::colorInterpretation);
        return new SolidDataset(cfg);
    } catch (const std::runtime_error & e) {
        CPLError(CE_Failure, CPLE_IllegalArg
                 , "SolidDataset initialization failure (%s).\n", e.what());
        return nullptr;
    }
}

SolidDataset::SolidDataset(const Config &config)
    : config_(config)
    , srs_(config.srs.as(geo::SrsDefinition::Type::wkt).srs)
{
    if (const auto *extents = config_.extents()) {
        const auto &e(*extents);
        auto es(math::size(e));

        geoTransform_[0] = e.ll(0);
        geoTransform_[1] = es.width / nRasterXSize;
        geoTransform_[2] = 0.0;

        geoTransform_[3] = e.ur(1);
        geoTransform_[4] = 0.0;
        geoTransform_[5] = -es.height / nRasterYSize;;
    } else if (const auto *geoTransform = config_.geoTransform()) {
        geoTransform_ = *geoTransform;
    }

    nRasterXSize = config_.size.width;
    nRasterYSize = config_.size.height;

    // prepare overviews
    std::vector<math::Size2> overviews;
    {
        auto size(config_.size);
        auto halve([&]()
        {
            size.width = int(std::round(size.width / 2.0));
            size.height = int(std::round(size.height / 2.0));
        });

        halve();
        while ((size.width >= config_.tileSize.width)
               || (size.height >= config_.tileSize.height))
        {
            overviews.push_back(size);
            halve();
        }
    }

    // NB: bands are 1-based, start with zero, pre-increment before setting band
    int i(0);
    for (const auto &band : config_.bands) {
        SetBand(++i, new RasterBand(this, band, overviews));
    }

}

CPLErr SolidDataset::GetGeoTransform(double *padfTransform)
{
    std::copy(geoTransform_.begin(), geoTransform_.end()
              , padfTransform);
    return CE_None;

    return CE_None;
}

const char* SolidDataset::GetProjectionRef()
{
    return srs_.c_str();
}

SolidDataset::RasterBand
::RasterBand(SolidDataset *dset , const Config::Band &band
             , const std::vector<math::Size2> &overviews)
    : block_(), blockSize_()
    , colorInterpretation_(band.colorInterpretation)
    , overviews_(overviews)
    , ovrBands_(overviews.size(), nullptr)
{
    const auto &cfg(dset->config_);
    poDS = dset;
    nBand = 1;
    nBlockXSize = cfg.tileSize.width;
    nBlockYSize = cfg.tileSize.height;
    eDataType = band.dataType;

    nRasterXSize = cfg.size.width;
    nRasterYSize = cfg.size.height;

    auto count(math::area(cfg.tileSize));

    switch (eDataType) {
    case ::GDT_Byte:
        createBlock<std::uint8_t>(band.value, count);
        break;

    case ::GDT_UInt16:
        createBlock<std::uint16_t>(band.value, count);
        break;

    case ::GDT_Int16:
        createBlock<std::int16_t>(band.value, count);
        break;

    case ::GDT_UInt32:
        createBlock<std::uint32_t>(band.value, count);
        break;

    case ::GDT_Int32:
        createBlock<std::int32_t>(band.value, count);
        break;

    case ::GDT_Float32:
        createBlock<float>(band.value, count);
        break;

    case ::GDT_Float64:
        createBlock<double>(band.value, count);
        break;

    default:
        utility::raise<std::runtime_error>
            ("Unsupported data type <%s>.", eDataType);
    };
}

CPLErr SolidDataset::RasterBand::IReadBlock(int, int, void *rawImage)
{
    // copy pregenerated data into output image
    std::memcpy(rawImage, block_.get(), blockSize_);
    return CE_None;
}

const math::Extents2* SolidDataset::Config::extents() const
{
    return boost::get<math::Extents2>(&extentsOrGeoTransform);
}

const geo::GeoTransform* SolidDataset::Config::geoTransform() const
{
    return boost::get<geo::GeoTransform>(&extentsOrGeoTransform);
}

void writeConfig(const boost::filesystem::path &file
                 , const SolidDataset::Config &config)
{
    std::ofstream f;
    f.exceptions(std::ios::badbit | std::ios::failbit);
    f.open(file.string(), std::ios_base::out | std::ios_base::trunc);

    f << std::scientific << std::setprecision(16);

    f << "[solid]"
      << "\nsrs = " << config.srs
      << "\nsize = " << config.size
      << "\ntileSize = " << config.tileSize
        ;

    if (const auto *extents = config.extents()) {
        f << "\nextents = " << *extents;
    } if (const auto *geoTransform = config.geoTransform()) {
        f << "\ngeoTransform = " << GeoTransformWrapper{*geoTransform};
    } else {
        LOGTHROW(err1, std::runtime_error)
            << "Neither extents nor geoTransform are set.";
    }
    f << "\n\n";

    f.unsetf(std::ios_base::floatfield);
    for (const auto &band : config.bands) {
        f << "\n[band]"
          << "\nvalue = " << band.value
          << "\ndataType = " << band.dataType
          << "\ncolorInterpretation = " << band.colorInterpretation
          << "\n";
    }

    f.close();
}

std::unique_ptr<SolidDataset>
SolidDataset::create(const boost::filesystem::path &path, const Config &config)
{
    std::unique_ptr<SolidDataset> solid(new SolidDataset(config));
    writeConfig(path, config);
    return solid;
}

} // namespace gdal_drivers

/* GDALRegister_SolidDataset */

void GDALRegister_SolidDataset()
{
    if (!GDALGetDriverByName("Solid")) {
        std::unique_ptr<GDALDriver> driver(new GDALDriver());

        driver->SetDescription("Solid");
        driver->SetMetadataItem
            (GDAL_DMD_LONGNAME
             , "Driver that returns solid valid in all pixels.");
        driver->SetMetadataItem(GDAL_DMD_EXTENSION, "");

        driver->pfnOpen = gdal_drivers::SolidDataset::Open;

        GetGDALDriverManager()->RegisterDriver(driver.release());
    }
}