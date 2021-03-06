///
/// Copyright 0000 <Nobody>
/// @file
/// @author David Matthew Mattli <dmattli@usgs.gov>
///
/// @section LICENSE
///
/// This software is in the public domain, furnished "as is", without
/// technical support, and with no warranty, express or implied, as to
/// its usefulness for any purpose.
///
/// @section DESCRIPTION
///
/// This file demonstrates how to use librasterblaster to implement parallel
/// raster reprojection. This implementation uses a MPI I/O to write to a tiff
/// file in parallel.
///
///

#include <sys/time.h>

#include <algorithm>
#include <vector>

#include "src/configuration.h"
#include "src/reprojection_tools.h"

#include "src/demos/sptw.h"
#include "src/utils.h"

using std::vector;

using librasterblaster::Area;
using librasterblaster::BlockPartition;
using librasterblaster::RasterChunk;
using librasterblaster::Configuration;
using librasterblaster::PRB_ERROR;
using librasterblaster::PRB_BADARG;
using librasterblaster::PRB_NOERROR;

using sptw::PTIFF;
using sptw::open_raster;
using sptw::SPTW_ERROR;

void MyErrorHandler(CPLErr eErrClass, int err_no, const char *msg) {
        return;
}
/*! \page prasterblasterpio

\htmlonly
USAGE:
\endhtmlonly

\verbatim
prasterblaster [--t_srs target_srs] [--s_srs source_srs] 
               [-r resampling_method] [-n partition_size]
               [--dstnodata no_data_value]
               source_file destination_file

\endverbatim

\section prasterblasterpio_description DESCRIPTION

<p> 

The prasterblasterpio demo program implements parallel raster reprojection and
demonstrates the use of librasterblaster. The implementation can be found in
prasterblaster-pio.cc.

</p>
 */
namespace librasterblaster {
PRB_ERROR write_rasterchunk(PTIFF *ptiff,
                             RasterChunk *chunk) {
  Area write_area;
  write_area.ul = chunk->ChunkToRaster(
      librasterblaster::Coordinate(0.0, 0.0, librasterblaster::UNDEF));
  write_area.lr = chunk->ChunkToRaster(
      librasterblaster::Coordinate(chunk->column_count_-1,
                                   chunk->row_count_-1,
                                   librasterblaster::UNDEF));
  sptw::write_area(ptiff,
                   chunk->pixels_,
                   write_area.ul.x,
                   write_area.ul.y,
                   write_area.lr.x,
                   write_area.lr.y);
  return PRB_NOERROR;
}

/** Main function for the prasterblasterpio program */
PRB_ERROR prasterblasterpio(Configuration conf) {
  RasterChunk *in_chunk, *out_chunk;
  double start_time, end_time, preloop_time;

  start_time = MPI_Wtime();

  int rank = 0;
  int process_count = 1;

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &process_count);

  // Replace CPLErrorHandler
  CPLPushErrorHandler(MyErrorHandler);

  GDALAllRegister();

  if (conf.input_filename == "" || conf.output_filename == "") {
    printf("USAGE:\n"
           "prasterblaster [--t_srs target_srs] [--s_srs source_srs]\n"
           "               [-r resampling_method] [-n partition_size]\n"
           "               [--dstnodata no_data_value]\n"
           "               [--timing-file filename]\n"
           "               [--tile-size tile_size_in_pixels]\n"
           "               source_file destination_file\n");
    return PRB_BADARG;
  }

  // Open the input raster
  //
  // The input raster is only read so we can use the serial i/o provided by the
  // GDAL library.
  GDALDataset *input_raster =
      static_cast<GDALDataset*>(GDALOpen(conf.input_filename.c_str(),
                                         GA_ReadOnly));
  if (input_raster == NULL) {
    fprintf(stderr, "Error opening input raster!\n");
    return PRB_IOERROR;
  }

  // If we are the process with rank 0 we are responsible for the creation of
  // the output raster.
  if (rank == 0) {
    OGRSpatialReference sr;
    char *wkt;
    sr.SetFromUserInput(input_raster->GetProjectionRef());
    sr.exportToPrettyWkt(&wkt);
    printf("prasterblaster-pio: Beginning reprojection task\n");
    printf("\tInput File: %s, Output File: %s\n",
           conf.input_filename.c_str(), conf.output_filename.c_str());
    OGRFree(wkt);

    // Now we have to create the output raster
    printf("Creating output raster...");
    double gt[6];
    input_raster->GetGeoTransform(gt);
    PRB_ERROR err = librasterblaster::CreateOutputRaster(input_raster,
                                                         conf.output_filename,
                                                         conf.output_srs,
                                                         conf.tile_size);
    if (err != PRB_NOERROR) {
      fprintf(stderr, "Error creating raster!: %d\n", err);
      return PRB_IOERROR;
    }
  }

  // Wait for rank 0 to finish creating the file
  MPI_Barrier(MPI_COMM_WORLD);
  PTIFF* output_raster = open_raster(conf.output_filename);
  if (rank == 0) {
    SPTW_ERROR sperr = populate_tile_offsets(output_raster,
                                             conf.tile_size,
                                             0);
    if (sperr != sptw::SP_None) {
      fprintf(stderr, "\nError populating tile offsets\n");
    }
    printf("done\n");
  }
  close_raster(output_raster);
  output_raster = open_raster(conf.output_filename);
  MPI_Barrier(MPI_COMM_WORLD);

  // Now open the new output file as a ProjectedRaster object. This object will
  // only be used to read metadata. It will _not_ be used to write to the output
  // file.
  GDALDataset *gdal_output_raster =
      static_cast<GDALDataset*>(GDALOpen(conf.output_filename.c_str(),
                                         GA_Update));

  if (output_raster == NULL) {
    fprintf(stderr, "Could not open output raster\n");
    MPI_Abort(MPI_COMM_WORLD, 1);
    return PRB_IOERROR;
  }

  vector<Area> partitions;

  partitions = BlockPartition(rank,
                              process_count,
                              output_raster->y_size,
                              output_raster->x_size,
                              output_raster->block_x_size,
                              conf.partition_size);

  if (rank == 0) {
    printf("Typical process has %lu partitions with base size: %d\n",
           static_cast<unsigned long>(partitions.size()),
           conf.partition_size);
  }
  double read_total, misc_start, misc_total;
  double write_start, write_end, write_total;
  double resample_start, resample_end, resample_total;
  double loop_start, prelude_end, minbox_total;

  read_total = write_total = resample_total = misc_total = minbox_total = 0.0;
  preloop_time = MPI_Wtime() - start_time;

  // Now we loop through the returned partitions
  for (size_t i = 0; i < partitions.size(); ++i) {
    loop_start = MPI_Wtime();

    // Now we use the ProjectedRaster object we created for the input file to
    // create a RasterChunk that has the pixel values read into it.
    in_chunk = RasterChunk::CreateRasterChunk(input_raster,
                                              gdal_output_raster,
                                              partitions.at(i));
    minbox_total += MPI_Wtime() - loop_start;

    prelude_end = MPI_Wtime();
    PRB_ERROR chunk_err = RasterChunk::ReadRasterChunk(input_raster, in_chunk);
    if (chunk_err != PRB_NOERROR) {
      fprintf(stderr, "Error reading input chunk!\n");
      MPI_Abort(MPI_COMM_WORLD, 1);
      return PRB_IOERROR;
    }
    read_total += MPI_Wtime() - prelude_end;

    misc_start = MPI_Wtime();
    // We want a RasterChunk for the output area but we area going to generate
    // the pixel values not read them from the file so we use
    // CreateRasterChunk
    out_chunk = RasterChunk::CreateRasterChunk(gdal_output_raster,
                                               partitions.at(i));
    if (out_chunk == NULL) {
      fprintf(stderr, "Error allocating output chunk! %f %f %f %f\n",
              partitions[i].ul.x,
              partitions[i].ul.y,
              partitions[i].lr.x,
              partitions[i].lr.y);
      return PRB_BADARG;
    }
    misc_total += MPI_Wtime() - misc_start;

    // Now we call ReprojectChunk with the RasterChunk pair and the desired
    // resampler. ReprojectChunk performs the reprojection/resampling and fills
    // the output RasterChunk with the new values.
    resample_start = MPI_Wtime();
    bool ret = ReprojectChunk(in_chunk,
                              out_chunk,
                              conf.fillvalue,
                              conf.resampler);
    if (ret == false) {
            fprintf(stderr, "Error reprojecting chunk!\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
    }
    resample_end = MPI_Wtime();
    resample_total += resample_end - resample_start;

    write_start = MPI_Wtime();
    PRB_ERROR err;

    err = write_rasterchunk(output_raster,
                            out_chunk);
    if (err != PRB_NOERROR) {
      fprintf(stderr, "Rank %d: Error writing chunk!\n", rank);
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
    write_end = MPI_Wtime();
    write_total += write_end - write_start;

    misc_start = MPI_Wtime();
    delete in_chunk;
    delete out_chunk;

    if (rank == 0) {
      printf(" %d%% ",
             (int)((i*100) / partitions.size()));
      fflush(stdout);
    }
    misc_total += MPI_Wtime() - misc_start;
  }

  if (rank == 0) {
    printf(" 100%%\n");
  }

  // Clean up
  write_start = MPI_Wtime();
  close_raster(output_raster);
  write_total += MPI_Wtime() - write_start;

  misc_start = MPI_Wtime();
  output_raster = NULL;
  delete gdal_output_raster;
  delete input_raster;
  misc_total += MPI_Wtime() - misc_start;

  // Report runtimes
  end_time = MPI_Wtime();
  double runtimes[7] = { end_time - start_time,
                         preloop_time,
                         minbox_total,
                         read_total,
                         resample_total,
                         write_total,
                         misc_total };
  std::vector<double> process_runtimes(process_count*7);
  MPI_Gather(runtimes,
             7,
             MPI_DOUBLE,
             &(process_runtimes[0]),
             7,
             MPI_DOUBLE,
             0,
             MPI_COMM_WORLD);
  double averages[7] = { 0.0 };

  for (unsigned int i = 0; i < process_runtimes.size(); i++) {
    averages[i % 7] += process_runtimes[i];
  }

  for (unsigned int i = 0; i < 7; i++) {
    averages[i] /= process_count;
  }
  if (rank == 0) {
    printf("Runtimes, in seconds\n");
    printf("Total  Pre-loop Minbox Read   Resample Write  Misc\n");
    printf("%.4f %.4f   %.4f %.4f %.4f   %.4f %.4f\n",
           averages[0],
           averages[1],
           averages[2],
           averages[3],
           averages[4],
           averages[5],
           averages[6]);
  }

  FILE *timing_file = stdout;
  if (rank == 0 && conf.timing_filename != "") {
    if (conf.timing_filename != "") {
      timing_file = fopen(conf.timing_filename.c_str(), "w");
      if (conf.timing_filename != "" && timing_file == NULL) {
        fprintf(stderr, "Error creating timing output file");
        timing_file = stdout;
      }
    }

    struct timeval time;
    gettimeofday(&time, NULL);
    fprintf(timing_file, "finish_time,process_count,total,preloop,minbox,read"
            ",resample,write,misc\n");
    fprintf(timing_file, "%lld,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
            static_cast<long long>(time.tv_sec),
            process_count,
            averages[0],
            averages[1],
            averages[2],
            averages[3],
            averages[4],
            averages[5],
            averages[6]);

    fprintf(timing_file, "process,total,preloop,minbox,read,resample"
            ",write,misc\n");
    for (unsigned int i = 0; i < process_runtimes.size(); i+=7) {
      fprintf(timing_file, "%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
              i/7,
              process_runtimes.at(i),
              process_runtimes.at(i+1),
              process_runtimes.at(i+2),
              process_runtimes.at(i+3),
              process_runtimes.at(i+4),
              process_runtimes.at(i+5),
              process_runtimes.at(i+6));
    }
  }
  if (rank == 0 && conf.timing_filename != "") {
    fclose(timing_file);
  }

  return PRB_NOERROR;
}
}
