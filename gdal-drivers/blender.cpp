/**
 * Copyright (c) 2019 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <cstdlib>
#include <algorithm>
#include <numeric>
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

#include "blender.hpp"

namespace po = boost::program_options;
namespace fs = boost::filesystem;

namespace gdal_drivers {

void writeConfig(const fs::path &file, const BlendingDataset::Config &config)
{
    std::ofstream f;
    f.exceptions(std::ios::badbit | std::ios::failbit);
    f.open(file.string(), std::ios_base::out | std::ios_base::trunc);

    f << "[blender]"
      << "\nsrs = " << config.srs
      << "\nextents = " << config.extents
      << "\noverlap = " << config.overlap
        ;

    if (config.resolution) {
        f  << "\nresolution = " << config.resolution.value();
    }

    f << "\n\n";

    for (const auto &ds : config.datasets) {
        f << "\n[dataset]"
          << "\npath = " << ds.path
          << "\nvalid = " << ds.valid
          << "\n";
    }

    f.close();
}

namespace {

struct ImageReference {
    fs::path path;
    cv::Rect extents;
    cv::Rect2d valid;

    ImageReference() = default;
    ImageReference(const fs::path &path, const cv::Rect &extents
                   , const cv::Rect2d &valid)
        : path(path), extents(extents), valid(valid)
    {}

    typedef std::vector<ImageReference> list;
};

const auto epsilon(1e-4);

bool almostSame(double a, double b)
{
    return (std::abs(a - b) < epsilon);
}

template <typename T>
bool almostSame(const math::Point2_<T> &a, const math::Point2_<T> &b)
{
    return almostSame(a(0), b(0)) && almostSame(a(1), b(1));
}

bool orthogonal(::GDALDataset *ds)
{
    geo::GeoTransform gt;
    ds->GetGeoTransform(gt.data());

    return (std::abs(gt[2]) < epsilon) && (std::abs(gt[4]) < epsilon);
}

math::Point2 getResolution(const geo::GeoTransform &gt)
{
    return { std::hypot(gt[1], gt[4]), std::hypot(gt[2], gt[5]) };
}

math::Point2 getResolution(::GDALDataset *ds)
{
    geo::GeoTransform gt;
    ds->GetGeoTransform(gt.data());
    return getResolution(gt);
}

void checkCompatibility(const fs::path &refPath, ::GDALDataset *ref
                        , const fs::path &dsPath, ::GDALDataset *ds)
{
    // orthogonality
    if (!orthogonal(ds)) {
        LOGTHROW( err2, std::runtime_error )
            << "Non-orthogonal GDAL dataset at " << dsPath
            << " cannot be georeferenced by extents.";
    }

    // resolution
    const auto rRef(getResolution(ref));
    const auto rDs(getResolution(ds));
    if (!almostSame(rRef, rDs)) {
        LOGTHROW( err2, std::runtime_error )
            << "GDAL dataset at " << dsPath << " has different resolution "
            << "(" << rDs
            << ") than reference raster dataset at " << refPath
            << " (" << rRef << ").";
    }

    // band compatibility
    if (ref->GetRasterCount() != ds->GetRasterCount()) {
        LOGTHROW( err2, std::runtime_error )
            << "GDAL dataset at " << dsPath << " has different number "
            << "of raster bands (" << ds->GetRasterCount()
            << ") than reference raster dataset at " << refPath
            << " (" << ref->GetRasterCount() << ").";
    }

    // TODO: check color interpretation
    // TODO: check data type
}

struct Descriptor {
    math::Extents2 extents;
    math::Size2 size;
    math::Point2d resolution;

    Descriptor() = default;
    Descriptor(::GDALDataset *ds);
    typedef std::vector<Descriptor> list;
};


Descriptor::Descriptor(::GDALDataset *ds)
    : size(ds->GetRasterXSize(), ds->GetRasterYSize())
    , resolution(getResolution(ds))
{
    geo::GeoTransform gt;
    ds->GetGeoTransform(gt.data());

    const auto &transform([&](double x, double y)
    {
        const auto &d(gt.data());
        return math::Point2(d[0] + x * d[1] + y * d[2]
                            , d[3] + x * d[4] + y * d[5]);
    });

    const auto ll(transform(0, size.height));
    const auto lr(transform(size.width, size.height));
    const auto ul(transform(0, 0));
    const auto ur(transform(size.width, 0));

    extents.ll(0) = std::min({ll(0), lr(0), ul(0), ur(0)});
    extents.ll(1) = std::min({ll(1), lr(1), ul(1), ur(1)});
    extents.ur(0) = std::max({ll(0), lr(0), ul(0), ur(0)});
    extents.ur(1) = std::max({ll(1), lr(1), ul(1), ur(1)});
}

} // namespace

/**
 * @brief BorderedAreaRasterBand
 */
class BlendingDataset::RasterBand : public ::GDALRasterBand {
public:
    RasterBand(BlendingDataset *dset, int bandIndex
               , const ImageReference::list &references);

    virtual ~RasterBand() {}

    virtual CPLErr IReadBlock(int nBlockXOff, int nBlockYOff, void *image);

    virtual GDALColorInterp GetColorInterpretation() {
        return bands_.front().band->GetColorInterpretation();
    }

    struct Band {
        typedef std::vector<Band> list;

        ::GDALRasterBand *band;
        ImageReference ref;

        Band(::GDALRasterBand *band, const ImageReference &ref)
            : band(band), ref(ref)
        {}
    };

private:
    /** List of source bands.
     */
    BlendingDataset *dset_;
    Band::list bands_;
};

BlendingDataset::BlendingDataset(const Config &config)
    : config_(config)
    , srs_(config.srs.as(geo::SrsDefinition::Type::wkt).srs)
{
    // open all datasets
    ::GDALDataset *main{};
    Descriptor des;
    Descriptor::list descriptors;

    for (auto &ds : config.datasets) {
        std::unique_ptr< ::GDALDataset> dset
            (static_cast< ::GDALDataset*>
             (::GDALOpen(ds.path.c_str(), GA_ReadOnly)));

        if (!dset) {
            LOGTHROW( err2, std::runtime_error )
                << "Failed to open dataset " << ds.path << ".";
        }

        descriptors.emplace_back(dset.get());

        if (datasets_.empty()) {
            main = dset.get();
            des = descriptors.back();
        } else {
            checkCompatibility(config.datasets.front().path, main
                               , ds.path, dset.get());
        }

        datasets_.push_back(std::move(dset));
    }

    ImageReference::list references;

    math::Point2 resolution;
    math::Point2 origin;

    {
        // align extents with dataset
        geo::GeoTransform gt;
        main->GetGeoTransform(gt.data());

        origin(0) = gt[0];
        origin(1) = gt[3];

        if (config.resolution) {
            // use provided resolution, no check is done
            resolution = { config.resolution->width
                           , config.resolution->height };
        } else {
            // use first dataset resolution
            resolution = getResolution(gt);
        }
    }

    const auto &align([&](const math::Extents2 &extents)
    {
        math::Extents2 res(extents.ll - origin, extents.ur - origin);
        for (int i = 0; i < 2; ++i) {
            res.ll(i) = ((std::floor(res.ll(i) / resolution(i))
                          * resolution(i) + origin(i)));
            res.ur(i) = ((std::ceil(res.ur(i) / resolution(i))
                          * resolution(i) + origin(i)));
        }
        return res;
    });

    // align extents
    const auto extents(align(config.extents));
    // LOG(info4) << std::fixed << "extents: " << extents;

    // compute raster size and set geo transform
    {
        const auto es(math::size(extents));
        nRasterXSize = es.width / resolution(0);
        nRasterYSize = es.height / resolution(1);

        geoTransform_[0] = extents.ll(0);
        geoTransform_[1] = resolution(0);
        geoTransform_[2] = 0.0;

        geoTransform_[3] = extents.ur(1);
        geoTransform_[4] = 0.0;
        geoTransform_[5] = -resolution(1);

        overlap_.width = config.overlap * resolution(0);
        overlap_.height = config.overlap * resolution(1);
    }

    const auto &point2pixel([&](const math::Point2 &p
                                , const math::Point2d &res)
    {
        const math::Point2 shiftd(p - ul(extents));
        return math::Point2i(shiftd(0) / res(0), -shiftd(1) / res(1));
    });

    const auto &pixelExtents([&](const math::Extents2 &e
                                 , const math::Size2 &size
                                 , const math::Point2d &res)
    {
        const auto ll(point2pixel(ul(e), res));
        return cv::Rect(ll(0), ll(1), size.width, size.height);
    });

    const auto &point2pixeld([&](const math::Point2d &p
                                 , const math::Point2d &res)
    {
        const math::Point2d shiftd(p - ul(extents));
        return math::Point2d(shiftd(0) / res(0), -shiftd(1) / res(1));
    });

    const auto &pixelValid([&](const math::Extents2 &e
                               , const math::Point2d &res)
    {
        const auto ll(point2pixeld(ul(e), res));
        const auto ur(point2pixeld(lr(e), res));
        return cv::Rect2d(ll(0), ll(1), ur(0) - ll(0), ur(1) - ll(1));
    });

    // compute references
    auto idescriptors(descriptors.begin());
    for (const auto &ds : config_.datasets) {
        const auto &des(*idescriptors++);

        references.emplace_back(ds.path
                                , pixelExtents(des.extents, des.size
                                               , resolution)
                                , pixelValid(ds.valid, resolution));

        // const auto &ref(references.back());
        // LOG(info4) << "ds: " << ds.path;
        // LOG(info4) << "    size: " << des.size;
        // LOG(info4) << std::fixed << "    extents: " << des.extents;
        // LOG(info4) << std::fixed << "    px extents: " << ref.extents;
        // LOG(info4) << std::fixed << "    px valid:  " << ref.valid;
    }

    // create bands

    std::size_t bandCount(main->GetRasterCount());
    for (std::size_t band(1); band <= bandCount; ++band) {
        SetBand(band, new RasterBand(this, band, references));
    }
}

CPLErr BlendingDataset::GetGeoTransform(double *padfTransform)
{
    std::copy(geoTransform_.begin(), geoTransform_.end()
              , padfTransform);
    return CE_None;
}

const char* BlendingDataset::GetProjectionRef()
{
    return srs_.c_str();
}

BlendingDataset::RasterBand
::RasterBand(BlendingDataset *dset, int bandIndex
             , const ImageReference::list &references)
    : dset_(dset)
{
    bands_.reserve(dset->datasets_.size());
    auto ireferences(references.begin());
    for (const auto &ds : dset->datasets_) {
        bands_.emplace_back(ds->GetRasterBand(bandIndex), *ireferences++);
    }

    nRasterXSize = dset->nRasterXSize;
    nRasterYSize = dset->nRasterXSize;

    nBlockXSize = 256;
    nBlockYSize = 256;
    eDataType = bands_.front().band->GetRasterDataType();
}

namespace {

int gdal2cv(GDALDataType type)
{
    switch (type) {
    case GDT_Byte: return CV_8UC1;
    case GDT_UInt16: return CV_16UC1;
    case GDT_Int16: return CV_16SC1;
    case GDT_UInt32: return CV_32SC1;
    case GDT_Int32: return CV_32SC1;
    case GDT_Float32: return CV_32FC1;
    case GDT_Float64: return CV_64FC1;

    default:
        LOGTHROW(err2, std::logic_error)
            << "Unsupported datatype " << type << " in raster.";
    }
    return 0; // never reached
}

} // namespace

CPLErr BlendingDataset::RasterBand::IReadBlock(int nBlockXOff, int nBlockYOff
                                               , void *rawImage)
{
    using Image = cv::Mat_<double>;

    cv::Rect block(nBlockXOff * nBlockXSize
                   , nBlockYOff * nBlockYSize
                   , nBlockXSize, nBlockYSize);

    // LOG(info4) << "reading block: [" << nBlockXOff << ", " << nBlockYOff
    //            << "]: " << block;

    Image acc(nBlockXSize, nBlockYSize, 0.0);
    Image wacc(nBlockXSize, nBlockYSize, 0.0);

    // for each band
    for (auto &band : bands_) {
        // compute source block
        auto roi(block & band.ref.extents);
        if (!roi.area()) { continue; }

        // LOG(info4) << "src: " << band.ref.path;
        // LOG(info4) << "extents: " << band.ref.extents;
        // LOG(info4) << "block: " << block;
        // LOG(info4) << "roi: " << roi;

        // localize
        const auto local(roi - band.ref.extents.tl());
        // LOG(info4) << "local: " << local;

        // view into block
        const auto origin(roi.tl() - block.tl());
        const cv::Rect view(origin.x, origin.y, local.width, local.height);
        // LOG(info4) << "origin: " << origin;
        // LOG(info4) << "view: " << view;

        Image image(local.size());

        // read block via generic RasterIO
        const auto err
            (band.band->RasterIO(GF_Read
                                 , local.x, local.y
                                 , local.width, local.height
                                 , image.data
                                 , local.width, local.height
                                 , GDT_Float64
                                 , image.elemSize(), image.step
                                 , nullptr));

        if (err != CE_None) { return err; }

        // get weights
        Image weights(local.size(), 1.0);
        if (!(band.band->GetMaskFlags() & GMF_ALL_VALID)) {
            // not all pixels valid, load weights from weights band
            auto mb(band.band->GetMaskBand());

            const auto err
                (mb->RasterIO(GF_Read
                              , local.x, local.y
                              , local.width, local.height
                              , weights.data
                              , local.width, local.height
                              , GDT_Float64
                              , weights.elemSize(), weights.step
                              , nullptr));
            if (err != CE_None) { return err; }
            for (auto &px : weights) { if (px) { px = 1.0; } }
        }

        // compute weight for each pixel
        const auto overlap(dset_->overlap_);

        if (math::empty(overlap)) {
            // no overlap, use only pixels inside the valid image area
            const auto &valid(band.ref.valid);
            cv::Point2f p(roi.x + 0.5, roi.y + 0.5);
            for (int j = 0; j < weights.rows; ++j, ++p.y) {
                auto pp(p);
                for (int i = 0; i < weights.cols; ++i, ++pp.x) {
                    // LOG(info4) << "px: " << i << "," << j;
                    // LOG(info4) << "valid: " << std::fixed
                    //            << valid.tl() << ":" << valid.br();
                    // LOG(info4) << "pp: " << std::fixed << pp;
                    // invalidate pixel ouside of valid area
                    if (!valid.contains(pp)) { weights(j, i) = 0.0; }
                }
            }
        } else {
            // apply overlap, take into acount pixels ouside the valid image
            // area
            const auto &valid(band.ref.valid);

            // full kernel area
            const auto kernelArea(4 * math::area(overlap));

            // base kernel at image's upper-left corner
            cv::Rect2d
                k(roi.x - overlap.width + 0.5
                  , roi.y - overlap.height + 0.5
                  , overlap.width * 2, overlap.height * 2);

            for (int j = 0; j < weights.rows; ++j, ++k.y) {
                auto kernel(k);
                for (int i = 0; i < weights.cols; ++i, ++kernel.x) {
                    // are of kernel clipped by "valid" extents
                    const auto area((valid & kernel).area());
                    // apply weight
                    weights(j, i) *= (area / kernelArea);
                }
            }
        }

        // add weighted data to accumulator
        cv::multiply(image, weights, image);
        Image(acc, view) += image;

        // update weight total
        Image(wacc, view) += weights;
    }

    // set weight for invaid pixels to 1 to not divide by zero
    wacc.setTo(1.0, (wacc == 0.0));
    // apply weights total to accumulated image
    acc /= wacc;

    {
        // copy data into output image
        const auto type(gdal2cv(bands_.front().band->GetRasterDataType()));
        cv::Mat out(nBlockYSize, nBlockXSize, type, rawImage);
        acc.convertTo(out, type);
    }
    return CE_None;
}

GDALDataset* BlendingDataset::Open(GDALOpenInfo *openInfo)
{
    ::CPLErrorReset();

    po::options_description config("blending GDAL driver");
    po::variables_map vm;
    Config cfg;

    config.add_options()
        ("blender.srs", po::value(&cfg.srs)->required()
         , "SRS definition. Use [WKT], +proj or EPSG:num.")
        ("blender.extents", po::value(&cfg.extents)->required()
         , "Geo extents of dataset (ulx,uly:urx,ury).")
        ("blender.overlap", po::value(&cfg.overlap)->required()
         , "Blending dataset overlap.")
        ("blender.resolution", po::value<math::Size2f>()
         , "Resolution of dataset. Defaults to first dataset resolution.")
        ;

    using utility::multi_value;

    config.add_options()
        ("dataset.path", multi_value<decltype(Config::Dataset::path)>()
         , "Value to return.")
        ("dataset.valid", multi_value<decltype(Config::Dataset::valid)>()
         , "Data type.")
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
                 "The Blending driver does not support update "
                 "access to existing datasets.\n");
        return nullptr;
    }

    // initialize dataset
    try {
        po::store(parsed, vm);
        po::notify(vm);

        if (vm.count("blender.resolution")) {
            cfg.resolution = vm["blender.resolution"].as<math::Size2f>();
        }

        // process bands
        auto &datasets(cfg.datasets);
        datasets.resize(vm["dataset.path"].as<std::vector<fs::path>>().size());
        using utility::process_multi_value;
        process_multi_value(vm, "dataset.path"
                            , datasets, &Config::Dataset::path);
        process_multi_value(vm, "dataset.valid"
                            , datasets, &Config::Dataset::valid);
        return new BlendingDataset(cfg);
    } catch (const std::runtime_error & e) {
        CPLError(CE_Failure, CPLE_IllegalArg
                 , "BlendingDataset initialization failure (%s).\n", e.what());
        return nullptr;
    }
}

std::unique_ptr<BlendingDataset>
BlendingDataset::create(const fs::path &path, const Config &config)
{
    std::unique_ptr<BlendingDataset> solid(new BlendingDataset(config));
    writeConfig(path, config);
    return solid;
}

} // namespace gdal_drivers

/* GDALRegister_BlendingDataset */

void GDALRegister_BlendingDataset()
{
    if (!GDALGetDriverByName("Blender")) {
        std::unique_ptr<GDALDriver> driver(new GDALDriver());

        driver->SetDescription("Blender");
        driver->SetMetadataItem
            (GDAL_DMD_LONGNAME
             , "Driver that blends multiple datasets into one.");
        driver->SetMetadataItem(GDAL_DMD_EXTENSION, "");

        driver->pfnOpen = gdal_drivers::BlendingDataset::Open;

        GetGDALDriverManager()->RegisterDriver(driver.release());
    }
}
