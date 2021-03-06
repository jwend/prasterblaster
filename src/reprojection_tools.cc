//
// Copyright 0000 <Nobody>
// @file
// @author David Matthew Mattli <dmattli@usgs.gov>
//
// @section LICENSE
//
// This software is in the public domain, furnished "as is", without
// technical support, and with no warranty, express or implied, as to
// its usefulness for any purpose.
//
// @section DESCRIPTION
//
// Helper functions to create and manipulate projections and projected rasters.
//
//

#include "src/reprojection_tools.h"

#include <float.h>

#include <ogr_api.h>
#include <ogr_spatialref.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <tiffio.h>

#include <algorithm>
#include <vector>
#include <ctime>
#include <cstdlib>
#include <sstream>

#include "src/rastercoordtransformer.h"
#include "src/resampler.h"
#include "src/std_int.h"
#include "src/utils.h"


namespace librasterblaster {
PRB_ERROR CreateOutputRaster(GDALDataset *in,
                             string output_filename,
                             string output_srs,
                             int output_tile_size) {
  OGRSpatialReference in_srs;
  OGRSpatialReference out_srs;
  OGRErr err;

  err = in_srs.SetFromUserInput(in->GetProjectionRef());
  if (err != OGRERR_NONE) {
    return PRB_BADARG;
  }

  err = out_srs.SetFromUserInput(output_srs.c_str());
  if (err != OGRERR_NONE) {
    return PRB_BADARG;
  }

  // Determine output raster size by calculating the projected coordinate minbox
  double in_transform[6];
  in->GetGeoTransform(in_transform);
  Coordinate ul(in_transform[0], in_transform[3], UNDEF);
  char *srs_str = NULL;
  in_srs.exportToProj4(&srs_str);
  Area out_area = ProjectedMinbox(ul,
                                  srs_str,
                                  in_transform[1],
                                  in->GetRasterYSize(),
                                  in->GetRasterXSize(),
                                  output_srs);
  CPLFree(srs_str);

  // Compute the distance, in the output projected coordinate units, from the
  // top corner of the transformed input space to the bottom corner of the
  // transformed input.
  const double delta_x = out_area.lr.x - out_area.ul.x;
  const double delta_y = out_area.lr.y - out_area.ul.y;
  const double diagonal_dist = sqrt(delta_x * delta_x + delta_y * delta_y);

  // Use this distance to compute a pixel size
  const double in_x_size = in->GetRasterBand(1)->GetXSize();
  const double in_y_size = in->GetRasterBand(1)->GetYSize();
  const double output_pixel_size = diagonal_dist
      / sqrt(in_x_size * in_x_size + in_y_size * in_y_size);

  const int64_t num_cols = static_cast<int64_t>(
      0.5 + ((out_area.lr.x - out_area.ul.x) / output_pixel_size));
  const int64_t num_rows = static_cast<int64_t>(
      0.5 + ((out_area.ul.y - out_area.lr.y) / output_pixel_size));

  PRB_ERROR result = CreateOutputRasterFile(in,
                                            output_filename,
                                            output_srs,
                                            num_cols,
                                            num_rows,
                                            output_pixel_size,
                                            out_area,
                                            output_tile_size);
  return result;
}

PRB_ERROR CreateOutputRaster(GDALDataset *in,
                             string output_filename,
                             string output_srs,
                             int output_tile_size,
                             int output_max_dimension) {

  OGRSpatialReference in_srs;
  OGRSpatialReference out_srs;
  OGRErr err;

  err = in_srs.SetFromUserInput(in->GetProjectionRef());
  if (err != OGRERR_NONE) {
    return PRB_BADARG;
  }

  err = out_srs.SetFromUserInput(output_srs.c_str());
  if (err != OGRERR_NONE) {
    return PRB_BADARG;
  }

   // Determine output raster size by calculating the projected coordinate minbox
  double in_transform[6];
  in->GetGeoTransform(in_transform);
  Coordinate ul(in_transform[0], in_transform[3], UNDEF);
  char *srs_str = NULL;
  in_srs.exportToProj4(&srs_str);
  Area out_area = ProjectedMinbox(ul,
                                  srs_str,
                                  in_transform[1],
                                  in->GetRasterYSize(),
                                  in->GetRasterXSize(),
                                  output_srs);
  CPLFree(srs_str);

  // Divide the both the x space and y space by the maximum output
  // dimension. This calculates a potential pixel size. Choose the largest pixel
  // size, that dimension (x or y) becomes output_max_dimension. The dimension
  // not chosen is then calculated with the generated pixel size.

  const double x_pixel_size = (out_area.lr.x - out_area.ul.x) / output_max_dimension;
  const double y_pixel_size = (out_area.ul.y - out_area.lr.y) / output_max_dimension;

  const double output_pixel_size = x_pixel_size > y_pixel_size ? x_pixel_size : y_pixel_size;

  const int64_t num_cols = static_cast<int64_t>(
      0.5 + ((out_area.lr.x - out_area.ul.x) / output_pixel_size));
  const int64_t num_rows = static_cast<int64_t>(
      0.5 + ((out_area.ul.y - out_area.lr.y) / output_pixel_size));

  PRB_ERROR result = CreateOutputRasterFile(in,
                                            output_filename,
                                            output_srs,
                                            num_cols,
                                            num_rows,
                                            output_pixel_size,
                                            out_area,
                                            output_tile_size);
  return result;
}

PRB_ERROR CreateOutputRasterFile(GDALDataset *in,
                                 string output_filename,
                                 string output_srs,
                                 int64_t output_columns,
                                 int64_t output_rows,
                                 double output_pixel_size,
                                 Area output_projected_area,
                                 int output_tile_size) {
  double in_transform[6];
  in->GetGeoTransform(in_transform);

  GDALAllRegister();
  GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");

  if (driver == NULL) {
    fprintf(stderr, "Error opening GTiff driver.\n");
    return PRB_BADARG;
  }

  std::stringstream ts;
  ts << output_tile_size;

  // Set driver options
  char **options = NULL;
  options = CSLSetNameValue(options, "INTERLEAVE", "PIXEL");
  options = CSLSetNameValue(options, "BIGTIFF", "YES");
  options = CSLSetNameValue(options, "TILED", "YES");
  options = CSLSetNameValue(options, "COMPRESS", "NONE");
  options = CSLSetNameValue(options, "BLOCKXSIZE", ts.str().c_str());
  options = CSLSetNameValue(options, "BLOCKYSIZE", ts.str().c_str());
  options = CSLSetNameValue(options, "SPARSE_OK", "YES");

  GDALDataset *output =
      driver->Create(output_filename.c_str(),
                     output_columns,
                     output_rows,
                     in->GetRasterCount(),
                     in->GetRasterBand(1)->GetRasterDataType(),
                     options);

  if (output == NULL) {
    fprintf(stderr, "driver->Create call failed.\n");
    return PRB_BADARG;
  }

  // Copy ColorTable
  if (in->GetRasterCount() > 0) {
    GDALColorTable *ct = in->GetRasterBand(1)->GetColorTable();
    output->GetRasterBand(1)->SetColorTable(ct);
  }

  // Setup georeferencing
  double out_t[6] = { output_projected_area.ul.x,
                      output_pixel_size,
                      0.0,
                      output_projected_area.ul.y,
                      0.0,
                      -output_pixel_size };

  output->SetGeoTransform(out_t);
  OGRSpatialReference out_sr;
  char *wkt = NULL;
  out_sr.SetFromUserInput(output_srs.c_str());
  out_sr.exportToWkt(&wkt);
  output->SetProjection(wkt);


  double *data = new double(sizeof(*data) * 4 * in->GetRasterCount());

  output->RasterIO(GF_Write,
                   0,
                   0,
                   1,
                   1,
                   data,
                   1,
                   1,
                   in->GetRasterBand(1)->GetRasterDataType(),
                   in->GetRasterCount(),
                   NULL,
                   0,
                   0,
                   0);

  output->RasterIO(GF_Write,
                   output_columns-1,
                   output_rows-1,
                   1,
                   1,
                   data,
                   1,
                   1,
                   in->GetRasterBand(1)->GetRasterDataType(),
                   in->GetRasterCount(),
                   NULL,
                   0,
                   0,
                   0);
  delete data;

  OGRFree(wkt);
  CSLDestroy(options);
  GDALClose(output);

  return PRB_NOERROR;
}

// For use by PartitionBySize
int simplerandom(int i) {
  return std::rand()%i;
}

bool partition_compare(Area a, Area b) {
  if (a.ul.y < b.ul.y) {
    return true;
  } else if (a.ul.y > b.ul.y) {
    return false;
  }
  else if (a.ul.x < b.ul.x) {
    return true;
  } else {
    return false;
  }
  return false;
}

std::vector<Area> BlockPartition(int rank,
                                 int process_count,
                                 int row_count,
                                 int column_count,
                                 int tile_size,
                                 int partition_size) {
  const int64_t partition_height = sqrt(partition_size);
  const int64_t partition_width = partition_size / partition_height;

  const int64_t tiles_down = (row_count + tile_size - 1) / tile_size;
  const int64_t tiles_across = (column_count + tile_size - 1) / tile_size;

  const int64_t partitions_down = (tiles_down + partition_height - 1) / partition_height;
  const int64_t partitions_across = (tiles_across + partition_width - 1) / partition_width;

  std::vector<Area> blocks;
  Area p;

  for (int64_t y = 0; y < partitions_down; ++y) {
    for (int64_t x = 0; x < partitions_across; ++x) {
      p.ul.x = x * partition_width;
      p.ul.y = y * partition_height;
      p.lr.x = p.ul.x + partition_width - 1;
      p.lr.y = p.ul.y + partition_height - 1;
      blocks.push_back(p);
    }
  }

  // Now we shuffle the partitions for load balancing.
  // All processes should generate the same shuffle.
  std::srand(42);
//  std::random_shuffle(blocks.begin(), blocks.end(), simplerandom);

  std::vector<Area> partitions;
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (i % process_count == static_cast<unsigned int>(rank)) {
      partitions.push_back(blocks.at(i));
    }
  }

  // Now scale the partitions by the tile size and verify they are within the
  // image bounds.
  for (size_t i = 0; i < partitions.size(); ++i) {
    partitions.at(i).ul.x *= tile_size;
    partitions.at(i).ul.y *= tile_size;

    partitions.at(i).lr.x *= tile_size;
    partitions.at(i).lr.y *= tile_size;

    partitions.at(i).lr.x += tile_size - 1;
    partitions.at(i).lr.y += tile_size - 1;

    if (partitions.at(i).lr.x > column_count) {
      partitions.at(i).lr.x = column_count - 1;
    }

    if (partitions.at(i).lr.y > row_count) {
      partitions.at(i).lr.y = row_count - 1;
    }
  }
  return partitions;
}

void SearchAndUpdate(Area input_area,
                     string input_srs,
                     string output_srs,
                     double input_ulx,
                     double input_uly,
                     double input_pixel_size,
                     Area *output_area) {
  Coordinate input_coord;
  Coordinate temp;
  OGRSpatialReference input_sr, output_sr;

  input_sr.SetFromUserInput(input_srs.c_str());
  output_sr.SetFromUserInput(output_srs.c_str());
  OGRCoordinateTransformation *t =
      OGRCreateCoordinateTransformation(&input_sr,
                                        &output_sr);

  if (t == NULL) {
          output_area->ul.x = -1.0;
          output_area->ul.y = -1.0;
          return;
  }

  for (int64_t x = input_area.ul.x; x <= input_area.lr.x; ++x) {
    for (int64_t y = input_area.ul.y; y >= input_area.lr.y; --y) {
      input_coord.x = x * input_pixel_size + input_ulx;
      input_coord.y = input_uly - (y * input_pixel_size);

      t->Transform(1, &input_coord.x, &input_coord.y);
      temp = input_coord;

      if (temp.x  < output_area->ul.x) {
        output_area->ul.x = temp.x;
      }
      if (temp.y > output_area->ul.y) {
        output_area->ul.y = temp.y;
      }
      if (temp.x > output_area->lr.x) {
        output_area->lr.x = temp.x;
      }

      if (temp.y < output_area->lr.y) {
        output_area->lr.y = temp.y;
      }
    }
  }

  OCTDestroyCoordinateTransformation(t);
  return;
}

Area ProjectedMinbox(Coordinate input_ul_corner,
                     string input_srs,
                     double input_pixel_size,
                     int input_row_count,
                     int input_column_count,
                     string output_srs) {
  // Input area, projected coordinates
  Area ia;
  // Projected Area
  Area output_area;
  const int buffer = 10;
  const int row_buffer = buffer > input_row_count ? input_row_count : buffer;
  const int column_buffer = buffer > input_column_count ? input_column_count : buffer;

  output_area.ul.x = output_area.lr.y = DBL_MAX;
  output_area.ul.y = output_area.lr.x = -DBL_MAX;

  // Check the top of the raster
  ia.ul.x = 0;
  ia.lr.x = input_column_count - 1;
  ia.ul.y = input_row_count - 1;
  ia.lr.y = input_row_count - row_buffer - 1;

  SearchAndUpdate(ia,
                  input_srs,
                  output_srs,
                  input_ul_corner.x,
                  input_ul_corner.y,
                  input_pixel_size,
                  &output_area);

  // Check the bottom of the raster
  ia.ul.x = 0;
  ia.lr.x = input_column_count - 1;
  ia.ul.y = input_row_count - row_buffer - 1;
  ia.lr.y = input_row_count - 1;

  SearchAndUpdate(ia,
                  input_srs,
                  output_srs,
                  input_ul_corner.x,
                  input_ul_corner.y,
                  input_pixel_size,
                  &output_area);

  // Check Left
  ia.ul.x = 0;
  ia.lr.x = column_buffer;
  ia.ul.y = input_row_count - 1;
  ia.lr.y = 0;

  SearchAndUpdate(ia,
                  input_srs,
                  output_srs,
                  input_ul_corner.x,
                  input_ul_corner.y,
                  input_pixel_size,
                  &output_area);

  // Check right
  ia.ul.x = input_column_count - column_buffer - 1;
  ia.lr.x = input_column_count - 1;
  ia.ul.y = input_row_count - 1;
  ia.lr.y = 0;

  SearchAndUpdate(ia,
                  input_srs,
                  output_srs,
                  input_ul_corner.x,
                  input_ul_corner.y,
                  input_pixel_size,
                  &output_area);

  return output_area;
}

Area RasterMinbox(GDALDataset *source,
                  GDALDataset *destination,
                  Area destination_raster_area) {
  double s_gt[6];
  double d_gt[6];
  source->GetGeoTransform(s_gt);
  destination->GetGeoTransform(d_gt);

  Coordinate s_ul(s_gt[0], s_gt[3], UNDEF);
  Coordinate d_ul(d_gt[0], d_gt[3], UNDEF);

  string s_srs(source->GetProjectionRef());
  string d_srs(destination->GetProjectionRef());

  return RasterMinbox2(s_srs,
                       s_ul,
                       s_gt[1],
                       source->GetRasterYSize(),
                       source->GetRasterXSize(),
                       d_srs,
                       d_ul,
                       d_gt[1],
                       destination->GetRasterYSize(),
                       destination->GetRasterXSize(),
                       destination_raster_area);
}

Area RasterMinbox2(string source_projection,
                  Coordinate source_ul,
                  double source_pixel_size,
                  int source_row_count,
                  int source_column_count,
                  string destination_projection,
                  Coordinate destination_ul,
                  double destination_pixel_size,
                  int destination_row_count,
                  int destination_column_count,
                  Area destination_raster_area) {
  Area source_area;
  Coordinate c;
  RasterCoordTransformer rt(source_projection,
                            source_ul,
                            source_pixel_size,
                            source_row_count,
                            source_column_count,
                            destination_projection,
                            destination_ul,
                            destination_pixel_size);
  if (rt.ready() == false) {
    return Area(-1.0, -1.0, -1.0, -1.0);
  }

  Area temp;
  source_area.ul.x = source_area.ul.y = DBL_MAX;
  source_area.lr.y = source_area.lr.x = -DBL_MAX;
  source_area.units = UNDEF;

  int row_space = destination_row_count / 10;
  if (row_space < 3) {
    row_space = destination_row_count;
  }

  int column_space = destination_column_count / 10;
  if (column_space < 3) {
    column_space = destination_column_count;
  }

  for (int y = destination_raster_area.ul.y;
       y <= destination_raster_area.lr.y; ++y) {
    for (int x = destination_raster_area.ul.x;
         x <= destination_raster_area.lr.x; ++x) {
      if (y > row_space
          && y < destination_column_count - row_space
          && x > column_space
          && x < destination_column_count - column_space) {
    }

      c.x = x;
      c.y = y;

      temp = rt.Transform(c);

      if (temp.ul.x == -1) {
        continue;
      }

      // Check that calculated minbox in within destination raster space.
      if ((temp.ul.x < -0.01) || (temp.ul.x > destination_column_count - 1)
          || (temp.ul.y < 0.0) || (temp.ul.y > destination_row_count - 1)
          || (temp.lr.x > destination_column_count - 1) || (temp.lr.x < 0.0)
          || (temp.lr.y > destination_row_count - 1) || (temp.lr.y < 0.0)) {
        temp.ul.x = -1.0;
        continue;
        printf("\n\nSearch Area: %f %f %f %f\n",
               destination_raster_area.ul.x,
               destination_raster_area.ul.y,
               destination_raster_area.lr.x,
               destination_raster_area.lr.y);
        printf("Source UL: %f %f\n", source_ul.x, source_ul.y);
        printf("Destin UL: %f %f\n", destination_ul.x, destination_ul.y);
        printf("Source raster size, columns: %d, rows %d\n",
               destination_column_count, destination_row_count);
        printf("Source: %d %d\n", x, y);
        printf("Outside rasterspace: %f %f %f %f\n",
               temp.ul.x, temp.ul.y, temp.lr.x, temp.lr.y);
      }

      if (temp.lr.x > source_area.lr.x) {
        source_area.lr.x = temp.lr.x;
      }

      if (temp.ul.x > source_area.lr.x) {
        source_area.lr.x = temp.ul.x;
      }

      if (temp.ul.x < source_area.ul.x) {
        source_area.ul.x = temp.ul.x;
      }

      if (temp.lr.x < source_area.ul.x) {
        source_area.ul.x = temp.lr.x;
      }

      if (temp.ul.y < source_area.ul.y) {
        source_area.ul.y = temp.ul.y;
      }

      if (temp.lr.y > source_area.lr.y) {
        source_area.lr.y = temp.lr.y;
      }
    }
  }

  // Check whether entire area is out of the projected space.
  if ((source_area.ul.x == DBL_MAX) || (source_area.ul.y == DBL_MAX)
      || (source_area.lr.x == -DBL_MAX) || (source_area.lr.y == -DBL_MAX)) {
    source_area.ul.x = -1.0;
    source_area.lr.x = -1.0;
    source_area.ul.y = -1.0;
    source_area.lr.y = -1.0;
    return source_area;
  }


  source_area.ul.x = floor(source_area.ul.x);
  source_area.ul.y = floor(source_area.ul.y);
  source_area.lr.x = ceil(source_area.lr.x);
  source_area.lr.y = ceil(source_area.lr.y);

  if (source_area.lr.x > destination_column_count - 1) {
    source_area.lr.x = destination_column_count - 1;
  }

  if (source_area.lr.y > destination_row_count - 1) {
    source_area.lr.y = destination_row_count - 1;
  }

  if (source_area.lr.y < source_area.ul.y) {
    source_area.lr.y = source_area.ul.y;
  }

  if (source_area.lr.x < source_area.ul.x) {
    source_area.lr.x = source_area.ul.x;
  }

  return source_area;
}

/**
 * \brief This function takes two RasterChunk pointers and performs
 *        reprojection and resampling
 * \param source Pointer to the RasterChunk to reproject from
 * \param destination Pointer to the RasterChunk to reproject to
 * \param fillvalue std::string with the fillvalue
 * \param resampler The resampler that should be used
 *
 * @return Returns a bool indicating success or failure.
 */
bool ReprojectChunk(RasterChunk *source,
                    RasterChunk *destination,
                    string fillvalue,
                    RESAMPLER resampler) {
  if (source->pixel_type_ != destination->pixel_type_) {
    fprintf(stderr, "Source and destination chunks have different types!\n");
    return false;
  }

  double fvalue = strtod(fillvalue.c_str(), NULL);

  switch (source->pixel_type_) {
    case GDT_Byte:
      GEN_RESAMPLER_CASES(uint8_t);
    case GDT_UInt16:
      GEN_RESAMPLER_CASES(uint16_t);
    case GDT_Int16:
      GEN_RESAMPLER_CASES(int16_t);
    case GDT_UInt32:
      GEN_RESAMPLER_CASES(uint32_t);
    case GDT_Int32:
      GEN_RESAMPLER_CASES(int32_t);
    case GDT_Float32:
      GEN_RESAMPLER_CASES(float);
    case GDT_Float64:
      GEN_RESAMPLER_CASES(double);
    case GDT_CInt16:
    case GDT_CInt32:
    case GDT_CFloat32:
    case GDT_CFloat64:
    default:
      fprintf(stderr, "Invalid type in ReprojectChunk!\n");
      return false;
      break;
  }
  return true;
}
}
