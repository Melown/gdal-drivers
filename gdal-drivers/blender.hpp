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

#ifndef gdal_drivers_blender_hpp_included_
#define gdal_drivers_blender_hpp_included_

#include <gdal_priv.h>

#include <memory>
#include <array>
#include <vector>

#include <boost/variant.hpp>
#include <boost/filesystem/path.hpp>

#include "math/geometry_core.hpp"
#include "geo/srsdef.hpp"
#include "geo/geotransform.hpp"

namespace gdal_drivers {

/**
 * @brief GttDataset
 */

class BlendingDataset : public ::GDALDataset {
public:
    static ::GDALDataset* Open(GDALOpenInfo *openInfo);

    virtual ~BlendingDataset() {};

    virtual CPLErr GetGeoTransform(double *padfTransform);
    virtual const char *GetProjectionRef();

    class Config {
    public:
        struct Dataset {
            boost::filesystem::path path;
            math::Extents2 inside;

            typedef std::vector<Dataset> list;
        };

        geo::SrsDefinition srs;
        math::Extents2 extents;
        double overlap = 0;
        Dataset::list datasets;
    };

    /** Creates new solid dataset and return pointer to it.
     */
    static std::unique_ptr<BlendingDataset>
    create(const boost::filesystem::path &path, const Config &config);

    BlendingDataset(const Config &config);

    typedef std::unique_ptr< ::GDALDataset> Dataset;
    typedef std::vector<Dataset> Datasets;

private:

    class RasterBand;
    friend class RasterBand;
    typedef std::vector<RasterBand> RasterBands;

    Config config_;
    std::string srs_;
    geo::GeoTransform geoTransform_;

    Datasets datasets_;
};

} // namespace gdal_drivers

// driver registration function
CPL_C_START
void GDALRegister_BlendingDataset(void);
CPL_C_END

#endif // gdal_drivers_blender_hpp_included_