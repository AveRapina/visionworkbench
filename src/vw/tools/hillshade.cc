// __BEGIN_LICENSE__
//  Copyright (c) 2006-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NASA Vision Workbench is licensed under the Apache License,
//  Version 2.0 (the "License"); you may not use this file except in
//  compliance with the License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__


#ifdef _MSC_VER
#pragma warning(disable:4244)
#pragma warning(disable:4267)
#pragma warning(disable:4996)
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <vw/Core/System.h>
#include <vw/Core/Log.h>
#include <vw/Core/FundamentalTypes.h>
#include <vw/Math/EulerAngles.h>
#include <vw/Image/Algorithms.h>
#include <vw/Image/ImageViewRef.h>
#include <vw/Image/Filter.h>
#include <vw/Image/PixelMask.h>
#include <vw/Image/MaskViews.h>
#include <vw/Image/PerPixelAccessorViews.h>
#include <vw/FileIO/DiskImageView.h>
#include <vw/FileIO/DiskImageResource.h>
#include <vw/Cartography/GeoReference.h>
#include <vw/tools/Common.h>

#include <boost/program_options.hpp>
#include <boost/filesystem/path.hpp>

namespace po = boost::program_options;
namespace fs = boost::filesystem;

using namespace vw;

struct Options {
  Options() : nodata_value(std::numeric_limits<double>::quiet_NaN()), blur_sigma(std::numeric_limits<double>::quiet_NaN()) {}
  // Input
  std::string input_file_name;

  // Settings
  std::string output_file_name;
  double azimuth, elevation, scale;
  double nodata_value;
  double blur_sigma;
};

// ----------------------------------------------------------------------------

template <class PixelT>
void do_hillshade(Options& opt) {

  cartography::GeoReference georef;
  cartography::read_georeference(georef, opt.input_file_name);

  // Select the pixel scale.
  float u_scale, v_scale;
  if (opt.scale == 0) {
    if (georef.is_projected()) {
      u_scale = georef.transform()(0,0);
      v_scale = georef.transform()(1,1);
    } else {
      double meters_per_degree = 2*M_PI*georef.datum().semi_major_axis()/360.0;
      u_scale = georef.transform()(0,0) * meters_per_degree;
      v_scale = georef.transform()(1,1) * meters_per_degree;
    }
  } else {
    u_scale = opt.scale;
    v_scale = -opt.scale;
  }

  // Set the direction of the light source.
  Vector3f light_0(1,0,0);
  Vector3f light = vw::math::euler_to_rotation_matrix(opt.elevation*M_PI/180, opt.azimuth*M_PI/180, 0, "yzx") * light_0;

  // Compute the surface normals
  vw_out() << "Loading: " << opt.input_file_name << ".\n";
  DiskImageView<PixelT > disk_dem_file(opt.input_file_name);

  ImageViewRef<PixelMask<PixelT > > dem;
  boost::scoped_ptr<SrcImageResource> disk_dem_rsrc(DiskImageResource::open(opt.input_file_name));
  if ( !std::isnan(opt.nodata_value) ) {
    vw_out() << "\t--> Masking pixel value: " << opt.nodata_value << ".\n";
    dem = create_mask(disk_dem_file, opt.nodata_value);
  } else if ( disk_dem_rsrc->has_nodata_read() ) {
    opt.nodata_value = disk_dem_rsrc->nodata_read();
    vw_out() << "\t--> Extracted nodata value from file: "
             << opt.nodata_value << ".\n";
    dem = create_mask(disk_dem_file, opt.nodata_value);
  } else {
    dem = pixel_cast<PixelMask<PixelT > >(disk_dem_file);
  }

  if ( !std::isnan(opt.blur_sigma) ) {
    vw_out() << "\t--> Blurring pixel with gaussian kernal.  Sigma = "
             << opt.blur_sigma << "\n";
    dem = gaussian_filter(dem, opt.blur_sigma);
  }

  // The final result is the dot product of the light source with the normals
  ImageViewRef<PixelMask<PixelGray<uint8> > > shaded_image =
    channel_cast_rescale<uint8>(clamp(dot_prod(compute_normals(dem, u_scale, v_scale), light)));

  // Save the result
  vw_out() << "Writing shaded relief image: " << opt.output_file_name << "\n";

  boost::scoped_ptr<DiskImageResource> r(DiskImageResource::create(opt.output_file_name,
                                                                   shaded_image.format()));
  if ( r->has_block_write() )
    r->set_block_write_size( Vector2i( vw_settings().default_tile_size(),
                                       vw_settings().default_tile_size() ) );
  write_georeference( *r, georef );
  block_write_image( *r, shaded_image,
                     TerminalProgressCallback( "tools.hillshade", "Writing:") );
}

void handle_arguments( int argc, char *argv[], Options& opt ) {
  po::options_description desc("Description: Outputs image of a DEM lighted as specified\n\nUsage: hillshade [options] <input file> \n\nOptions");
  desc.add_options()
    ("input-file", po::value(&opt.input_file_name), "Explicitly specify the input file")
    ("output-file,o", po::value(&opt.output_file_name), "Specify the output file")
    ("azimuth,a", po::value(&opt.azimuth)->default_value(0), "Sets the direction tha the light source is coming from (in degrees).  Zero degrees is to the right, with positive degree counter-clockwise.")
    ("elevation,e", po::value(&opt.elevation)->default_value(45), "Set the elevation of the light source (in degrees).")
    ("scale,s", po::value(&opt.scale)->default_value(0), "Set the scale of a pixel (in the same units as the DTM height values.")
    ("nodata-value", po::value(&opt.nodata_value), "Remap the DEM default value to the min altitude value.")
    ("blur", po::value(&opt.blur_sigma), "Pre-blur the DEM with the specified sigma.")
    ("help,h", "Display this help message");

  po::positional_options_description p;
  p.add("input-file", 1);

  po::variables_map vm;
  try {
    po::store( po::command_line_parser( argc, argv ).options(desc).positional(p).run(), vm );
    po::notify( vm );
  } catch (const po::error& e) {
    vw_throw( ArgumentErr() << "Error parsing input:\n\t"
              << e.what() << desc );
  }

  if( vm.count("help") )
    vw_throw( ArgumentErr() << desc );

  if ( vm.count("input-file") != 1 )
    vw_throw( ArgumentErr() << "Error: Must specify exactly one input file!\n\n" << desc );

  if ( opt.output_file_name.empty() )
    opt.output_file_name =
      fs::path(opt.input_file_name).replace_extension().string() + "_HILLSHADE.tif";
}

int main( int argc, char *argv[] ) {

  Options opt;
  try {
    handle_arguments( argc, argv, opt );

    ImageFormat fmt = tools::image_format(opt.input_file_name);

    switch(fmt.pixel_format) {
    case VW_PIXEL_GRAY:
    case VW_PIXEL_GRAYA:
      switch (fmt.channel_type) {
      case VW_CHANNEL_UINT8:  do_hillshade<PixelGray<uint8>  >( opt ); break;
      case VW_CHANNEL_INT16:  do_hillshade<PixelGray<int16>  >( opt ); break;
      case VW_CHANNEL_UINT16: do_hillshade<PixelGray<uint16> >( opt ); break;
      default:                do_hillshade<PixelGray<float>  >( opt ); break;
      }
      break;
    default:
      vw_throw( ArgumentErr() << "Unsupported pixel format. The DEM image must have only one channel." );
    }
  } catch ( const ArgumentErr& e ) {
    vw_out() << e.what() << std::endl;
    return 1;
  } catch ( const Exception& e ) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
