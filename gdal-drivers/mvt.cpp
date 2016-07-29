/*
 * @file mvt.cpp
 */

#include <cstdlib>
#include <algorithm>
#include <vector>
#include <iterator>
#include <fstream>
#include <iomanip>

#include <boost/lexical_cast.hpp>

#include <ogrsf_frmts.h>

#include "dbglog/dbglog.hpp"

#include "utility/raise.hpp"
#include "utility/multivalue.hpp"
#include "utility/streams.hpp"
#include "geo/gdal.hpp"
#include "geo/po.hpp"

#include "./mvt.hpp"

namespace po = boost::program_options;

namespace gdal_drivers {

/** Not using matrix: saving multiplications since there is no rotation.
 */
class Trafo {
public:
    Trafo(double extent, const boost::optional<math::Extents2> &extents) {
        if (extents) {
            // use upper-left corner
            shift_ = math::ul(*extents);
            scale_ = math::size(*extents);

            // scale must be negative in Y axis
            scale_.width /= extent;
            scale_.height /= -extent;
        } else {
            shift_(1) = 1.0;
            scale_.width = 1.0 / extent;
            scale_.height = 1.0 / extent;
        }
    }
#if 0
    inline double x(std::uint32_t value) const {
        return value * scale_.width + shift_(0);
    }

    inline double y(std::uint32_t value) const {
        return value * scale_.height + shift_(1);
    }
#endif
    inline double x(std::uint32_t value) const { return value; }

    inline double y(std::uint32_t value) const { return value; }

private:
    math::Point2d shift_;
    math::Size2f scale_;
};

class MvtDataset::Layer : public ::OGRLayer
{
public:
    Layer(MvtDataset &ds, const vector_tile::Tile_Layer &layer)
        : ds_(ds), layer_(layer)
        , featureDefn_
          (::OGRFeatureDefn::CreateFeatureDefn(layer_.name().c_str()))
        , ifeatures_(layer_.features().begin())
        , efeatures_(layer_.features().end())
        , trafo_(layer_.extent(), ds_.extents_)
    {
        if (ds_.srs_) {
            srs_.reset(new ::OGRSpatialReference(ds.srs_->reference()));
        }
    }

    virtual ~Layer() {
        featureDefn_->Release();
    }

    virtual ::OGRSpatialReference* GetSpatialRef() { return srs_.get(); }
    virtual void ResetReading();
    virtual ::OGRFeature* GetNextFeature();
    virtual ::OGRFeatureDefn* GetLayerDefn() { return featureDefn_; }
    virtual int TestCapability(const char*) { return 0; }
    virtual const char* GetName() { return layer_.name().c_str(); }
    virtual ::GIntBig GetFeatureCount(int force);

private:
    MvtDataset &ds_;
    std::unique_ptr< ::OGRSpatialReference> srs_;
    const vector_tile::Tile_Layer &layer_;
    ::OGRFeatureDefn *featureDefn_;
    decltype(layer_.features().begin()) ifeatures_;
    decltype(layer_.features().end()) efeatures_;
    Trafo trafo_;
};

::GIntBig MvtDataset::Layer::GetFeatureCount(int) {
    return layer_.features_size();
}

void MvtDataset::Layer::ResetReading()
{
    ifeatures_ = layer_.features().begin();
}

namespace {

OGRwkbGeometryType type(vector_tile::Tile_GeomType type)
{
    switch (type) {
    case vector_tile::Tile_GeomType::Tile_GeomType_POINT:
        return ::OGRwkbGeometryType::wkbPoint;

    case vector_tile::Tile_GeomType::Tile_GeomType_LINESTRING:
        return ::OGRwkbGeometryType::wkbLineString;

    case vector_tile::Tile_GeomType::Tile_GeomType_POLYGON:
        return ::OGRwkbGeometryType::wkbPolygon;

    default: break;
    }

    return ::OGRwkbGeometryType::wkbUnknown;
}

typedef decltype(vector_tile::Tile_Feature().geometry()) MvtGeometry;

struct Cursor {
    std::uint32_t x;
    std::uint32_t y;

    Cursor() : x(), y() {}
};

class GeometryReader : public Trafo {
public:
    GeometryReader(const Trafo &trafo, const MvtGeometry &source)
        : Trafo(trafo)
        , source_(source), pos_(source_.begin()), end_(source_.end())
    {}

    struct Command {
        enum class Type {  moveTo = 1, lineTo = 2, closePath = 7 };
        Type type;
        std::uint32_t count;

        Command(const std::uint32_t &raw)
            : type(static_cast<Type>(raw & 0x7)), count(raw >> 3)
        {}
    };

    operator bool() const { return pos_ != end_; }

    Command command(Command::Type type) {
        if (pos_ == end_) {
            LOGTHROW(err1, std::runtime_error)
                << "Cannot read command past the end of input.";
        }
        Command c(*pos_++);
        if (c.type != type) {
            LOGTHROW(err1, std::runtime_error)
                << "Unexpected type: " << static_cast<int>(c.type)
                << " (expected: " << static_cast<int>(type) << ").";
        }
        return c;
    }

    void shift(Cursor &cursor) {
        if (pos_ == end_) {
            LOGTHROW(err1, std::runtime_error)
                << "Cannot read shift past the end of input.";
        }
        cursor.x += *pos_++;

        if (pos_ == end_) {
            LOGTHROW(err1, std::runtime_error)
                << "Cannot read shift past the end of input.";
        }
        cursor.y += *pos_++;
    }

private:
    const MvtGeometry &source_;
    decltype(source_.begin()) pos_;
    decltype(source_.end()) end_;
};


GeometryReader::Command checkNonzero(const GeometryReader::Command &cmd)
{
    if (!cmd.count) {
        LOGTHROW(err1, std::runtime_error)
            << "Expected nonzero count, got " << cmd.count << ".";
    }
    return cmd;
}

GeometryReader::Command checkZero(const GeometryReader::Command &cmd)
{
    if (cmd.count) {
        LOGTHROW(err1, std::runtime_error)
            << "Expected zero count, got " << cmd.count << ".";
    }
    return cmd;
}

GeometryReader::Command checkSingle(const GeometryReader::Command &cmd)
{
    if (cmd.count != 1) {
        LOGTHROW(err1, std::runtime_error)
            << "Expected single count, got " << cmd.count << ".";
    }
    return cmd;
}

std::unique_ptr< ::OGRGeometry> points(GeometryReader &gr)
{
    Cursor cur;

    // moveTo+
    auto moveTo
        (checkNonzero(gr.command(GeometryReader::Command::Type::moveTo)));

    if (moveTo.count == 1) {
        // single point
        gr.shift(cur);
        return std::unique_ptr< ::OGRPoint>
            (new ::OGRPoint(gr.x(cur.x), gr.y(cur.y)));
    }

    // multi point
    std::unique_ptr< ::OGRGeometryCollection> g(new ::OGRGeometryCollection());

    // process all points
    while (moveTo.count--) {
        gr.shift(cur);
        g->addGeometryDirectly(new ::OGRPoint(gr.x(cur.x), gr.y(cur.y)));
    }

    return std::move(g);
}

template <typename Type = ::OGRLineString>
std::unique_ptr<Type>
singleLineString(GeometryReader &gr, Cursor &cur, bool closed = false)
{
    std::unique_ptr<Type> ls(new Type());

    // moveTo{1}
    auto moveTo
        (checkSingle(gr.command(GeometryReader::Command::Type::moveTo)));

    gr.shift(cur);
    ls->addPoint(gr.x(cur.x), gr.y(cur.y));
    auto start(cur);

    // lineTo+
    auto lineTo
        (checkNonzero(gr.command(GeometryReader::Command::Type::lineTo)));

    while (lineTo.count--) {
        gr.shift(cur);
        ls->addPoint(gr.x(cur.x), gr.y(cur.y));
    }

    if (!closed) { return ls; }

    // expect closePath{1}
    auto closePath
        (checkNonzero(gr.command(GeometryReader::Command::Type::closePath)));

    // last segment
    ls->addPoint(gr.x(start.x), gr.y(start.y));

    return ls;
}

std::unique_ptr< ::OGRGeometry> lineStrings(GeometryReader &gr)
{
    Cursor cur;

    std::unique_ptr< ::OGRLineString> single;
    std::unique_ptr< ::OGRMultiLineString> multi;

    while (gr) {
        if (single) {
            // another line string, create multi line string and add
            multi.reset(new ::OGRMultiLineString());
            multi->addGeometryDirectly(single.release());
        }

        single = singleLineString(gr, cur);

        if (multi) {
            // already building multi line string, add
            multi->addGeometryDirectly(single.release());
        }
    }

    // single or multi?
    if (single) { return std::move(single); }
    return std::move(multi);
}

std::unique_ptr< ::OGRGeometry> polygons(GeometryReader &gr)
{
    Cursor cur;

    std::unique_ptr< ::OGRPolygon> single;
    std::unique_ptr< ::OGRMultiPolygon> multi;

    while (gr) {
        // single linear ring
        auto ls(singleLineString< ::OGRLinearRing>(gr, cur, true));
        if (ls->isClockwise()) {
            // exterior ring
            if (single) {
                // previous polygon was first
                if (!multi) { multi.reset(new ::OGRMultiPolygon()); }
                // add new full polygon
                multi->addGeometryDirectly(single.release());
            }
        }

        if (!single) {
            // create new polygon
            single.reset(new OGRPolygon());
        }

        // add current ring
        single->addRingDirectly(ls.release());
    }

    // handle last addition
    if (multi && single) {
        multi->addGeometryDirectly(single.release());
    }

    // single or multi?
    if (single) { return std::move(single); }
    return std::move(multi);
}

std::unique_ptr< ::OGRGeometry>
generateGeometry(const vector_tile::Tile_Feature &feature, const Trafo &trafo)
{
    GeometryReader gr(trafo, feature.geometry());
    switch (feature.type()) {
    case vector_tile::Tile_GeomType::Tile_GeomType_POINT:
        return points(gr);

    case vector_tile::Tile_GeomType::Tile_GeomType_LINESTRING:
        return lineStrings(gr);

    case vector_tile::Tile_GeomType::Tile_GeomType_POLYGON:
        return polygons(gr);

    default: break;
    }

    // should be never reached
    return {};
}

} // namespace

::OGRFeature* MvtDataset::Layer::GetNextFeature()
{
    // skip unknown feature
    while ((ifeatures_ != efeatures_)
           && (ifeatures_->type()
               == vector_tile::Tile_GeomType::Tile_GeomType_UNKNOWN))
    {
        ++ifeatures_;
    }
    if (ifeatures_ == efeatures_) { return nullptr; }

    // valid feature
    const auto &feature(*ifeatures_);

    auto *defn(new ::OGRFeatureDefn());
    defn->SetGeomType(type(feature.type()));

    // create feature
    std::unique_ptr< ::OGRFeature> of(new ::OGRFeature(defn));

    // set ID
    if (feature.has_id()) { of->SetFID(feature.id()); }

    try {
        // set geometry
        auto geometry(generateGeometry(feature, trafo_));
        if (srs_) { geometry->assignSpatialReference(srs_.get()); }
        of->SetGeometryDirectly(geometry.release());
    } catch (const std::exception &e) {
        CPLError(CE_Failure, CPLE_AssertionFailed
                 , "Error processing feature's geometry: <%s>.\n"
                 , e.what());
        return nullptr;
    }

    // next
    ++ifeatures_;
    return of.release();
}

struct GetValue {
    GetValue(const vector_tile::Tile_Value &value) : value(value) {}
    const vector_tile::Tile_Value &value;
};

std::ostream& operator<<(std::ostream &os, const GetValue &gv)
{
    if (gv.value.has_string_value()) { return os << gv.value.string_value(); }
    if (gv.value.has_float_value()) { return os << gv.value.float_value(); }
    if (gv.value.has_double_value()) { return os << gv.value.double_value(); }
    if (gv.value.has_int_value()) { return os << gv.value.int_value(); }
    if (gv.value.has_uint_value()) { return os << gv.value.uint_value(); }
    if (gv.value.has_sint_value()) { return os << gv.value.sint_value(); }
    if (gv.value.has_bool_value()) { return os << gv.value.bool_value(); }
    return os;
}

MvtDataset::MvtDataset(std::unique_ptr<vector_tile::Tile> tile
                       , const boost::optional<geo::SrsDefinition> &srs
                       , const boost::optional<math::Extents2> &extents)
    : tile_(std::move(tile)), srs_(srs), extents_(extents)
    , layers_(tile_->layers_size())
{}

OGRLayer* MvtDataset::GetLayer(int l)
{
    if ((l < 0) || (l >= int(layers_.size()))) { return nullptr; }
    auto &layer(layers_[l]);

    // create if not created yet
    if (!layer) { layer.reset(new Layer(*this, tile_->layers(l))); }
    return layer.get();
}

OGRLayer* MvtDataset::GetLayerByName(const char *name)
{
    auto ilayers(layers_.begin());
    for (const auto &l : tile_->layers()) {
        if (l.name() == name) {
            // create if not created yet
            if (!*ilayers) { ilayers->reset(new Layer(*this, l)); }
            return ilayers->get();

        }
        ++ilayers;
    }
    // not found
    return nullptr;
}

GDALDataset* MvtDataset::Open(::GDALOpenInfo *openInfo)
{
    ::CPLErrorReset();

    // TODO: detect

    // open

    std::unique_ptr<vector_tile::Tile> tile(new vector_tile::Tile());

    try {
        std::ifstream f(openInfo->pszFilename
                        , std::ios::in | std::ios::binary);
        if (!tile->ParseFromIstream(&f)) {
            return nullptr;
        }
    } catch (...) {
        return nullptr;
    }

    if (openInfo->eAccess == GA_Update) {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "MVT driver allows only read-only access.\n");
    }

    boost::optional<geo::SrsDefinition> srs;

    if (const char *mvtSrs
        = ::CSLFetchNameValue(openInfo->papszOpenOptions, "MVT_SRS"))
    {
        try {
            srs = geo::SrsDefinition::fromString(mvtSrs);
        } catch (const std::exception &e) {
            CPLError(CE_Failure, CPLE_IllegalArg
                     , "MVT Dataset initialization failure: "
                     "failed to parse provided open options MVT_SRS (%s).\n"
                     , e.what());
            return nullptr;
        }
    }

    boost::optional<math::Extents2> extents;
    if (const char *mvtExtents
        = ::CSLFetchNameValue(openInfo->papszOpenOptions, "MVT_EXTENTS"))
    {
        try {
            extents = boost::lexical_cast<math::Extents2>(mvtExtents);
        } catch (std::exception) {
            CPLError(CE_Failure, CPLE_IllegalArg
                     , "MVT Dataset initialization failure: "
                     "failed to parse provided open options MVT_EXTENTS.\n");
            return nullptr;
        }
    }

    // parsed tile, pass it to dataset
    return new MvtDataset(std::move(tile), srs, extents);
}

} // namespace gdal_drivers

/* GDALRegister_MvtDataset */

void GDALRegister_MvtDataset()
{
    if (GDALGetDriverByName("MVT")) { return; }

    std::unique_ptr< ::GDALDriver> driver(new ::GDALDriver());

    driver->SetDescription("MVT");
    driver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    driver->SetMetadataItem
        (GDAL_DMD_LONGNAME, "Mapbox Vector TIles.");
    driver->SetMetadataItem(GDAL_DMD_EXTENSION, "");

    driver->pfnOpen = gdal_drivers::MvtDataset::Open;
    driver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    ::GetGDALDriverManager()->RegisterDriver(driver.release());
}
