#include "trirectangle.h"
#include "cg.h"
#include "pixelgrid.h"
#include "profile.h"
#include "sbprofile.h"
#include "qlens.h"
#include "mathexpr.h"
#include "errors.h"
#include <vector>
#include <complex>
#include <stdio.h>

#ifdef USE_MKL
#include "mkl.h"
#include "mkl_spblas.h"
#endif

#ifdef USE_UMFPACK
#include "umfpack.h"
#endif

#ifdef USE_FITS
#include "fitsio.h"
#endif

#ifdef USE_FFTW
#ifdef USE_MKL
#include "fftw/fftw3.h"
#else
#include "fftw3.h"
#endif
#endif

#include <cmath>
#include <iostream>
#include <fstream>
#include <iomanip>
#define USE_COMM_WORLD -987654
#define MUMPS_SILENT -1
#define MUMPS_OUTPUT 6
#define JOB_INIT -1
#define JOB_END -2
using namespace std;

int SourcePixelGrid::nthreads = 0;
const int SourcePixelGrid::max_levels = 6;
int SourcePixelGrid::number_of_pixels;
int *SourcePixelGrid::imin, *SourcePixelGrid::imax, *SourcePixelGrid::jmin, *SourcePixelGrid::jmax;
TriRectangleOverlap *SourcePixelGrid::trirec = NULL;
InterpolationCells *SourcePixelGrid::nearest_interpolation_cells = NULL;
lensvector **SourcePixelGrid::interpolation_pts[3];
//int *SourcePixelGrid::n_interpolation_pts = NULL;

int DelaunayGrid::nthreads = 0;
lensvector **DelaunayGrid::interpolation_pts[3] = { NULL, NULL, NULL};
//ImagePixelGrid* DelaunayGrid::image_pixel_grid = NULL;

// The following should probably just be private, local variables in the relevant functions, that have to keep getting set from the lens pointers.
// Otherwise it will be bug prone whenever changes are made, since the zfactors/betafactors pointers may be deleted and reassigned
double *SourcePixelGrid::srcgrid_zfactors = NULL;
double *ImagePixelGrid::imggrid_zfactors = NULL;
double **SourcePixelGrid::srcgrid_betafactors = NULL;
double **ImagePixelGrid::imggrid_betafactors = NULL;

// parameters for creating the recursive grid
double SourcePixelGrid::xcenter, SourcePixelGrid::ycenter;
double SourcePixelGrid::srcgrid_xmin, SourcePixelGrid::srcgrid_xmax, SourcePixelGrid::srcgrid_ymin, SourcePixelGrid::srcgrid_ymax;
int SourcePixelGrid::u_split_initial, SourcePixelGrid::w_split_initial;
double SourcePixelGrid::min_cell_area;

// variables for root finding to get point images (for combining with extended pixel images)
int ImagePixelGrid::nthreads = 0;
bool *ImagePixelGrid::newton_check = NULL;
lensvector *ImagePixelGrid::fvec = NULL;
double ImagePixelGrid::image_pos_accuracy = 1e-6; // default

// NOTE!!! It would be better to make a few of these (e.g. levels) non-static and contained in the zeroth-level grid, and just give all the subcells a pointer to the zeroth-level grid.
// That way, you can create multiple source grids and they won't interfere with each other.
int SourcePixelGrid::levels, SourcePixelGrid::splitlevels;
//lensvector SourcePixelGrid::d1, SourcePixelGrid::d2, SourcePixelGrid::d3, SourcePixelGrid::d4;
//double SourcePixelGrid::product1, SourcePixelGrid::product2, SourcePixelGrid::product3;
ImagePixelGrid* SourcePixelGrid::image_pixel_grid = NULL;
bool SourcePixelGrid::regrid;
int *SourcePixelGrid::maxlevs = NULL;
lensvector ***SourcePixelGrid::xvals_threads = NULL;
lensvector ***SourcePixelGrid::corners_threads = NULL;
lensvector **SourcePixelGrid::twistpts_threads = NULL;
int **SourcePixelGrid::twist_status_threads = NULL;

bool QLens::setup_fft_convolution;
double *QLens::psf_zvec;
int QLens::fft_imin, QLens::fft_jmin, QLens::fft_ni, QLens::fft_nj;
#ifdef USE_FFTW
fftw_plan QLens::fftplan, QLens::fftplan_inverse;
fftw_plan *QLens::fftplans_Lmatrix, *QLens::fftplans_Lmatrix_inverse;
complex<double> *QLens::psf_transform, *QLens::img_transform;
complex<double> **QLens::Lmatrix_transform;
double *QLens::img_rvec;
double **QLens::Lmatrix_imgs_rvec;
#endif
bool QLens::setup_fft_convolution_emask;
double *QLens::psf_zvec_emask;
int QLens::fft_imin_emask, QLens::fft_jmin_emask, QLens::fft_ni_emask, QLens::fft_nj_emask;
#ifdef USE_FFTW
fftw_plan QLens::fftplan_emask, QLens::fftplan_inverse_emask;
fftw_plan *QLens::fftplans_Lmatrix_emask, *QLens::fftplans_Lmatrix_inverse_emask;
complex<double> *QLens::psf_transform_emask, *QLens::img_transform_emask;
complex<double> **QLens::Lmatrix_transform_emask;
double *QLens::img_rvec_emask;
double **QLens::Lmatrix_imgs_rvec_emask;
#endif

ifstream SourcePixelGrid::sb_infile;

/***************************************** Multithreaded variables in class ImagePixelGrid ****************************************/

void ImagePixelGrid::allocate_multithreaded_variables(const int& threads, const bool reallocate)
{
	if (newton_check != NULL) {
		if (!reallocate) return;
		else deallocate_multithreaded_variables();
	}
	nthreads = threads;
	newton_check = new bool[threads];
	fvec = new lensvector[threads];
}

void ImagePixelGrid::deallocate_multithreaded_variables()
{
	if (newton_check != NULL) {
		delete[] newton_check;
		delete[] fvec;
		newton_check = NULL;
		fvec = NULL;
	}
}

/***************************************** Functions in class SourcePixelGrid ****************************************/

void SourcePixelGrid::set_splitting(int usplit0, int wsplit0, double min_cs)
{
	u_split_initial = usplit0;
	w_split_initial = wsplit0;
	if ((u_split_initial < 2) or (w_split_initial < 2)) die("source grid dimensions cannot be smaller than 2 along either direction");
	min_cell_area = min_cs;
}

void SourcePixelGrid::allocate_multithreaded_variables(const int& threads, const bool reallocate)
{
	if (trirec != NULL) {
		if (!reallocate) return;
		else deallocate_multithreaded_variables();
	}
	nthreads = threads;
	trirec = new TriRectangleOverlap[nthreads];
	imin = new int[nthreads];
	imax = new int[nthreads];
	jmin = new int[nthreads];
	jmax = new int[nthreads];
	nearest_interpolation_cells = new InterpolationCells[nthreads];
	int i,j;
	for (i=0; i < 3; i++) interpolation_pts[i] = new lensvector*[nthreads];
	//n_interpolation_pts = new int[threads];
	maxlevs = new int[threads];
	xvals_threads = new lensvector**[threads];
	for (j=0; j < threads; j++) {
		xvals_threads[j] = new lensvector*[3];
		for (i=0; i <= 2; i++) xvals_threads[j][i] = new lensvector[3];
	}
	corners_threads = new lensvector**[nthreads];
	for (int i=0; i < nthreads; i++) corners_threads[i] = new lensvector*[4];
	twistpts_threads = new lensvector*[nthreads];
	twist_status_threads = new int*[nthreads];
}

void SourcePixelGrid::deallocate_multithreaded_variables()
{
	if (trirec != NULL) {
		delete[] trirec;
		delete[] imin;
		delete[] imax;
		delete[] jmin;
		delete[] jmax;
		delete[] nearest_interpolation_cells;
		delete[] maxlevs;
		for (int i=0; i < 3; i++) delete[] interpolation_pts[i];
		//delete[] n_interpolation_pts;
		int i,j;
		for (j=0; j < nthreads; j++) {
			for (i=0; i <= 2; i++) delete[] xvals_threads[j][i];
			delete[] xvals_threads[j];
			delete[] corners_threads[j];
		}
		delete[] xvals_threads;
		delete[] corners_threads;
		delete[] twistpts_threads;
		delete[] twist_status_threads;

		trirec = NULL;
		imin = NULL;
		imax = NULL;
		jmin = NULL;
		jmax = NULL;
		nearest_interpolation_cells = NULL;
		maxlevs = NULL;
		for (int i=0; i < 3; i++) interpolation_pts[i] = NULL;
		//n_interpolation_pts = NULL;
		xvals_threads = NULL;
		corners_threads = NULL;
		twistpts_threads = NULL;
		twist_status_threads = NULL;
	}
}

SourcePixelGrid::SourcePixelGrid(QLens* lens_in, double x_min, double x_max, double y_min, double y_max) : lens(lens_in)	// use for top-level cell only; subcells use constructor below
{
	int threads = 1;
#ifdef USE_OPENMP
	#pragma omp parallel
	{
		#pragma omp master
		threads = omp_get_num_threads();
	}
#endif
	allocate_multithreaded_variables(threads,false); // allocate multithreading arrays ONLY if it hasn't been allocated already (avoids seg faults)

// this constructor is used for a Cartesian grid
	center_pt = 0;
	// For the Cartesian grid, u = x, w = y
	u_N = u_split_initial;
	w_N = w_split_initial;
	level = 0;
	levels = 0;
	ii=jj=0;
	cell = NULL;
	parent_cell = NULL;
	maps_to_image_pixel = false;
	maps_to_image_window = false;
	active_pixel = false;
	srcgrid_zfactors = lens->reference_zfactors;
	srcgrid_betafactors = lens->default_zsrc_beta_factors;

	for (int i=0; i < 4; i++) {
		corner_pt[i]=0;
		neighbor[i]=NULL;
	}

	xcenter = 0.5*(x_min+x_max);
	ycenter = 0.5*(y_min+y_max);
	srcgrid_xmin = x_min; srcgrid_xmax = x_max;
	srcgrid_ymin = y_min; srcgrid_ymax = y_max;

	double x, y, xstep, ystep;
	xstep = (x_max-x_min)/u_N;
	ystep = (y_max-y_min)/w_N;

	lensvector **firstlevel_xvals = new lensvector*[u_N+1];
	int i,j;
	for (i=0, x=x_min; i <= u_N; i++, x += xstep) {
		firstlevel_xvals[i] = new lensvector[w_N+1];
		for (j=0, y=y_min; j <= w_N; j++, y += ystep) {
			firstlevel_xvals[i][j][0] = x;
			firstlevel_xvals[i][j][1] = y;
		}
	}

	cell = new SourcePixelGrid**[u_N];
	for (i=0; i < u_N; i++)
	{
		cell[i] = new SourcePixelGrid*[w_N];
		for (j=0; j < w_N; j++)
		{
			cell[i][j] = new SourcePixelGrid(lens,firstlevel_xvals,i,j,1,this);
		}
	}
	levels++;
	assign_firstlevel_neighbors();
	number_of_pixels = u_N*w_N;
	for (int i=0; i < u_N+1; i++)
		delete[] firstlevel_xvals[i];
	delete[] firstlevel_xvals;
}

SourcePixelGrid::SourcePixelGrid(QLens* lens_in, string pixel_data_fileroot, const double& minarea_in) : lens(lens_in)	// use for top-level cell only; subcells use constructor below
{
	int threads = 1;
#ifdef USE_OPENMP
	#pragma omp parallel
	{
		#pragma omp master
		threads = omp_get_num_threads();
	}
#endif
	allocate_multithreaded_variables(threads,false); // allocate multithreading arrays ONLY if it hasn't been allocated already (avoids seg faults)

	min_cell_area = minarea_in;
	string info_filename = pixel_data_fileroot + ".info";
	ifstream infofile(info_filename.c_str());
	double cells_per_pixel;
	infofile >> u_split_initial >> w_split_initial >> cells_per_pixel;
	infofile >> srcgrid_xmin >> srcgrid_xmax >> srcgrid_ymin >> srcgrid_ymax;

	// this constructor is used for a Cartesian grid
	center_pt = 0;
	// For the Cartesian grid, u = x, w = y
	u_N = u_split_initial;
	w_N = w_split_initial;
	level = 0;
	levels = 0;
	ii=jj=0;
	cell = NULL;
	parent_cell = NULL;
	maps_to_image_pixel = false;
	maps_to_image_window = false;
	active_pixel = false;
	srcgrid_zfactors = lens->reference_zfactors;
	srcgrid_betafactors = lens->default_zsrc_beta_factors;

	for (int i=0; i < 4; i++) {
		corner_pt[i]=0;
		neighbor[i]=NULL;
	}

	xcenter = 0.5*(srcgrid_xmin+srcgrid_xmax);
	ycenter = 0.5*(srcgrid_ymin+srcgrid_ymax);

	double x, y, xstep, ystep;
	xstep = (srcgrid_xmax-srcgrid_xmin)/u_N;
	ystep = (srcgrid_ymax-srcgrid_ymin)/w_N;

	lensvector **firstlevel_xvals = new lensvector*[u_N+1];
	int i,j;
	for (i=0, x=srcgrid_xmin; i <= u_N; i++, x += xstep) {
		firstlevel_xvals[i] = new lensvector[w_N+1];
		for (j=0, y=srcgrid_ymin; j <= w_N; j++, y += ystep) {
			firstlevel_xvals[i][j][0] = x;
			firstlevel_xvals[i][j][1] = y;
		}
	}

	cell = new SourcePixelGrid**[u_N];
	for (i=0; i < u_N; i++)
	{
		cell[i] = new SourcePixelGrid*[w_N];
		for (j=0; j < w_N; j++)
		{
			cell[i][j] = new SourcePixelGrid(lens,firstlevel_xvals,i,j,1,this);
		}
	}
	levels++;
	assign_firstlevel_neighbors();
	number_of_pixels = u_N*w_N;

	string sbfilename = pixel_data_fileroot + ".sb";
	sb_infile.open(sbfilename.c_str());
	read_surface_brightness_data();
	sb_infile.close();
	for (int i=0; i < u_N+1; i++)
		delete[] firstlevel_xvals[i];
	delete[] firstlevel_xvals;
}

// ***NOTE: the following constructor should NOT be used because there are static variables (e.g. levels), so more than one source grid
// is a bad idea. To make this work, you need to make those variables non-static and contained in the zeroth-level grid (and give subcells
// a pointer to the zeroth-level grid).
SourcePixelGrid::SourcePixelGrid(QLens* lens_in, SourcePixelGrid* input_pixel_grid) : lens(lens_in)	// use for top-level cell only; subcells use constructor below
{
	int threads = 1;
#ifdef USE_OPENMP
	#pragma omp parallel
	{
		#pragma omp master
		threads = omp_get_num_threads();
	}
#endif
	allocate_multithreaded_variables(threads,false); // allocate multithreading arrays ONLY if it hasn't been allocated already (avoids seg faults)

	// these are all static anyway, so this might be superfluous
	min_cell_area = input_pixel_grid->min_cell_area;
	u_split_initial = input_pixel_grid->u_split_initial;
	w_split_initial = input_pixel_grid->w_split_initial;
	srcgrid_xmin = input_pixel_grid->srcgrid_xmin;
	srcgrid_xmax = input_pixel_grid->srcgrid_xmax;
	srcgrid_ymin = input_pixel_grid->srcgrid_ymin;
	srcgrid_ymax = input_pixel_grid->srcgrid_ymax;

	// this constructor is used for a Cartesian grid
	center_pt = 0;
	// For the Cartesian grid, u = x, w = y
	u_N = u_split_initial;
	w_N = w_split_initial;
	level = 0;
	levels = 0;
	ii=jj=0;
	cell = NULL;
	parent_cell = NULL;
	maps_to_image_pixel = false;
	maps_to_image_window = false;
	active_pixel = false;

	for (int i=0; i < 4; i++) {
		corner_pt[i]=0;
		neighbor[i]=NULL;
	}

	xcenter = 0.5*(srcgrid_xmin+srcgrid_xmax);
	ycenter = 0.5*(srcgrid_ymin+srcgrid_ymax);

	double x, y, xstep, ystep;
	xstep = (srcgrid_xmax-srcgrid_xmin)/u_N;
	ystep = (srcgrid_ymax-srcgrid_ymin)/w_N;

	lensvector **firstlevel_xvals = new lensvector*[u_N+1];
	int i,j;
	for (i=0, x=srcgrid_xmin; i <= u_N; i++, x += xstep) {
		firstlevel_xvals[i] = new lensvector[w_N+1];
		for (j=0, y=srcgrid_ymin; j <= w_N; j++, y += ystep) {
			firstlevel_xvals[i][j][0] = x;
			firstlevel_xvals[i][j][1] = y;
		}
	}

	cell = new SourcePixelGrid**[u_N];
	for (i=0; i < u_N; i++)
	{
		cell[i] = new SourcePixelGrid*[w_N];
		for (j=0; j < w_N; j++)
		{
			cell[i][j] = new SourcePixelGrid(lens,firstlevel_xvals,i,j,1,this);
		}
	}
	levels++;
	assign_firstlevel_neighbors();
	number_of_pixels = u_N*w_N;
	copy_source_pixel_grid(input_pixel_grid); // this copies the surface brightnesses and subpixel_maps_to_srcpixel the source pixels in the same manner as the input grid
	assign_all_neighbors();

	for (int i=0; i < u_N+1; i++)
		delete[] firstlevel_xvals[i];
	delete[] firstlevel_xvals;
}

void SourcePixelGrid::read_surface_brightness_data()
{
	double sb;
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			sb_infile >> sb;
			if (sb==-1e30) // I can't think of a better dividing value to use right now, so -1e30 is what I am using at the moment
			{
				cell[i][j]->split_cells(2,2,0);
				cell[i][j]->read_surface_brightness_data();
			} else {
				cell[i][j]->surface_brightness = sb;
			}
		}
	}
}

void SourcePixelGrid::copy_source_pixel_grid(SourcePixelGrid* input_pixel_grid)
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (input_pixel_grid->cell[i][j]->cell != NULL) {
				cell[i][j]->split_cells(input_pixel_grid->cell[i][j]->u_N,input_pixel_grid->cell[i][j]->w_N,0);
				cell[i][j]->copy_source_pixel_grid(input_pixel_grid->cell[i][j]);
			} else {
				cell[i][j]->surface_brightness = input_pixel_grid->cell[i][j]->surface_brightness;
			}
		}
	}
}

SourcePixelGrid::SourcePixelGrid(QLens* lens_in, lensvector** xij, const int& i, const int& j, const int& level_in, SourcePixelGrid* parent_ptr)
{
	u_N = 1;
	w_N = 1;
	level = level_in;
	cell = NULL;
	ii=i; jj=j; // store the index carried by this cell in the grid of the parent cell
	parent_cell = parent_ptr;
	maps_to_image_pixel = false;
	maps_to_image_window = false;
	active_pixel = false;
	surface_brightness = 0;
	lens = lens_in;

	corner_pt[0] = xij[i][j];
	corner_pt[1] = xij[i][j+1];
	corner_pt[2] = xij[i+1][j];
	corner_pt[3] = xij[i+1][j+1];

	center_pt[0] = (corner_pt[0][0] + corner_pt[1][0] + corner_pt[2][0] + corner_pt[3][0]) / 4.0;
	center_pt[1] = (corner_pt[0][1] + corner_pt[1][1] + corner_pt[2][1] + corner_pt[3][1]) / 4.0;
	find_cell_area();
}

void SourcePixelGrid::assign_surface_brightness_from_analytic_source()
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->assign_surface_brightness_from_analytic_source();
			else {
				cell[i][j]->surface_brightness = 0;
				for (int k=0; k < lens->n_sb; k++) {
					if (lens->sb_list[k]->is_lensed) cell[i][j]->surface_brightness += lens->sb_list[k]->surface_brightness(cell[i][j]->center_pt[0],cell[i][j]->center_pt[1]);
				}
			}
		}
	}
}

void SourcePixelGrid::assign_surface_brightness_from_delaunay_grid(DelaunayGrid* delaunay_grid)
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->assign_surface_brightness_from_delaunay_grid(delaunay_grid);
			else {
				cell[i][j]->surface_brightness = delaunay_grid->find_lensed_surface_brightness(cell[i][j]->center_pt,-1,-1,0); // it would be nice to use Greg's method for searching so it doesn't start from an arbitrary triangle...but it's pretty fast as-is
			}
		}
	}
}

void SourcePixelGrid::update_surface_brightness(int& index)
{
	for (int j=0; j < w_N; j++) {
		for (int i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->update_surface_brightness(index);
			else {
				if (cell[i][j]->active_pixel) {
					cell[i][j]->surface_brightness = lens->source_pixel_vector[index++];
				} else {
					cell[i][j]->surface_brightness = 0;
				}
			}
		}
	}
}

void SourcePixelGrid::fill_surface_brightness_vector()
{
	int column_j = 0;
	fill_surface_brightness_vector_recursive(column_j);
}

void SourcePixelGrid::fill_surface_brightness_vector_recursive(int& column_j)
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->fill_surface_brightness_vector_recursive(column_j);
			else {
				if (cell[i][j]->active_pixel) {
					lens->source_pixel_vector[column_j++] = cell[i][j]->surface_brightness;
				}
			}
		}
	}
}

void SourcePixelGrid::fill_n_image_vector()
{
	int column_j = 0;
	fill_n_image_vector_recursive(column_j);
}

void SourcePixelGrid::fill_n_image_vector_recursive(int& column_j)
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->fill_n_image_vector_recursive(column_j);
			else {
				if (cell[i][j]->active_pixel) {
					lens->source_pixel_n_images[column_j++] = cell[i][j]->n_images;
				}
			}
		}
	}
}

void SourcePixelGrid::find_avg_n_images()
{
	// no support for adaptive grid in this function, which is ok since we're only using this when Cartesian sources are not being used

	lens->max_pixel_sb=-1e30;
	int max_sb_i, max_sb_j;
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->surface_brightness > lens->max_pixel_sb) {
				lens->max_pixel_sb = cell[i][j]->surface_brightness;
				max_sb_i = i;
				max_sb_j = j;
			}
		}
	}

	lens->n_images_at_sbmax = cell[max_sb_i][max_sb_j]->n_images;
	lens->pixel_avg_n_image = 0;
	double sbtot = 0;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->surface_brightness >= lens->max_pixel_sb*lens->n_image_prior_sb_frac) {
				lens->pixel_avg_n_image += cell[i][j]->n_images*cell[i][j]->surface_brightness;
				sbtot += cell[i][j]->surface_brightness;
			}
		}
	}
	if (sbtot != 0) lens->pixel_avg_n_image /= sbtot;
}

ofstream SourcePixelGrid::pixel_surface_brightness_file;
ofstream SourcePixelGrid::pixel_magnification_file;
ofstream SourcePixelGrid::pixel_n_image_file;

void SourcePixelGrid::store_surface_brightness_grid_data(string root)
{
	string img_filename = root + ".sb";
	string info_filename = root + ".info";

	pixel_surface_brightness_file.open(img_filename.c_str());
	write_surface_brightness_to_file();
	pixel_surface_brightness_file.close();

	ofstream pixel_info; lens->open_output_file(pixel_info,info_filename);
	pixel_info << u_split_initial << " " << w_split_initial << " " << levels << endl;
	pixel_info << srcgrid_xmin << " " << srcgrid_xmax << " " << srcgrid_ymin << " " << srcgrid_ymax << endl;
}

void SourcePixelGrid::write_surface_brightness_to_file()
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) {
				pixel_surface_brightness_file << "-1e30\n";
				cell[i][j]->write_surface_brightness_to_file();
			} else {
				pixel_surface_brightness_file << cell[i][j]->surface_brightness << endl;
			}
		}
	}
}

void SourcePixelGrid::get_grid_dimensions(double &xmin, double &xmax, double &ymin, double &ymax)
{
	xmin = cell[0][0]->corner_pt[0][0];
	ymin = cell[0][0]->corner_pt[0][1];
	xmax = cell[u_N-1][w_N-1]->corner_pt[3][0];
	ymax = cell[u_N-1][w_N-1]->corner_pt[3][1];
}

void SourcePixelGrid::plot_surface_brightness(string root)
{
	string img_filename = root + ".dat";
	string x_filename = root + ".x";
	string y_filename = root + ".y";
	string info_filename = root + ".info";
	string mag_filename = root + ".maglog";
	string n_image_filename = root + ".nimg";

	double x, y, cell_xlength, cell_ylength, xmin, ymin;
	int i, j, k, n_plot_xcells, n_plot_ycells, pixels_per_cell_x, pixels_per_cell_y;
	cell_xlength = cell[0][0]->corner_pt[2][0] - cell[0][0]->corner_pt[0][0];
	cell_ylength = cell[0][0]->corner_pt[1][1] - cell[0][0]->corner_pt[0][1];
	n_plot_xcells = u_N;
	n_plot_ycells = w_N;
	pixels_per_cell_x = 1;
	pixels_per_cell_y = 1;
	for (i=0; i < levels-1; i++) {
		cell_xlength /= 2;
		cell_ylength /= 2;
		n_plot_xcells *= 2;
		n_plot_ycells *= 2;
		pixels_per_cell_x *= 2;
		pixels_per_cell_y *= 2;
	}
	xmin = cell[0][0]->corner_pt[0][0];
	ymin = cell[0][0]->corner_pt[0][1];

	ofstream pixel_xvals; lens->open_output_file(pixel_xvals,x_filename);
	for (i=0, x=xmin; i <= n_plot_xcells; i++, x += cell_xlength) pixel_xvals << x << endl;

	ofstream pixel_yvals; lens->open_output_file(pixel_yvals,y_filename);
	for (i=0, y=ymin; i <= n_plot_ycells; i++, y += cell_ylength) pixel_yvals << y << endl;

	lens->open_output_file(pixel_surface_brightness_file,img_filename.c_str());
	lens->open_output_file(pixel_magnification_file,mag_filename.c_str());
	if (lens->n_image_prior) lens->open_output_file(pixel_n_image_file,n_image_filename.c_str());
	int line_number;
	for (j=0; j < w_N; j++) {
		for (line_number=0; line_number < pixels_per_cell_y; line_number++) {
			for (i=0; i < u_N; i++) {
				if (cell[i][j]->cell != NULL) {
					cell[i][j]->plot_cell_surface_brightness(line_number,pixels_per_cell_x,pixels_per_cell_y);
				} else {
					for (k=0; k < pixels_per_cell_x; k++) {
						pixel_surface_brightness_file << cell[i][j]->surface_brightness << " ";
						pixel_magnification_file << log(cell[i][j]->total_magnification)/log(10) << " ";
						if (lens->n_image_prior) pixel_n_image_file << cell[i][j]->n_images << " ";
					}
				}
			}
			pixel_surface_brightness_file << endl;
			pixel_magnification_file << endl;
			if (lens->n_image_prior) pixel_n_image_file << endl;
		}
	}
	pixel_surface_brightness_file.close();
	pixel_magnification_file.close();
	pixel_n_image_file.close();

	ofstream pixel_info; lens->open_output_file(pixel_info,info_filename);
	pixel_info << u_split_initial << " " << w_split_initial << " " << levels << endl;
	pixel_info << srcgrid_xmin << " " << srcgrid_xmax << " " << srcgrid_ymin << " " << srcgrid_ymax << endl;
}

void SourcePixelGrid::plot_cell_surface_brightness(int line_number, int pixels_per_cell_x, int pixels_per_cell_y)
{
	int cell_row, subplot_pixels_per_cell_x, subplot_pixels_per_cell_y, subline_number=line_number;
	subplot_pixels_per_cell_x = pixels_per_cell_x/u_N;
	subplot_pixels_per_cell_y = pixels_per_cell_y/w_N;
	cell_row = line_number / subplot_pixels_per_cell_y;
	subline_number -= cell_row*subplot_pixels_per_cell_y;

	int i,j;
	for (i=0; i < u_N; i++) {
		if (cell[i][cell_row]->cell != NULL) {
			cell[i][cell_row]->plot_cell_surface_brightness(subline_number,subplot_pixels_per_cell_x,subplot_pixels_per_cell_y);
		} else {
			for (j=0; j < subplot_pixels_per_cell_x; j++) {
				pixel_surface_brightness_file << cell[i][cell_row]->surface_brightness << " ";
				pixel_magnification_file << log(cell[i][cell_row]->total_magnification)/log(10) << " ";
				if (lens->n_image_prior) pixel_n_image_file << cell[i][cell_row]->n_images << " ";
			}
		}
	}
}

inline void SourcePixelGrid::find_cell_area()
{
	//d1[0] = corner_pt[2][0] - corner_pt[0][0]; d1[1] = corner_pt[2][1] - corner_pt[0][1];
	//d2[0] = corner_pt[1][0] - corner_pt[0][0]; d2[1] = corner_pt[1][1] - corner_pt[0][1];
	//d3[0] = corner_pt[2][0] - corner_pt[3][0]; d3[1] = corner_pt[2][1] - corner_pt[3][1];
	//d4[0] = corner_pt[1][0] - corner_pt[3][0]; d4[1] = corner_pt[1][1] - corner_pt[3][1];
	// split cell into two triangles; cross product of the vectors forming the legs gives area of each triangle, so their sum gives area of cell
	//cell_area = 0.5 * (abs(d1 ^ d2) + abs(d3 ^ d4)); // overkill since the cells are just square
	cell_area = (corner_pt[2][0] - corner_pt[0][0])*(corner_pt[1][1]-corner_pt[0][1]);
}

void SourcePixelGrid::assign_firstlevel_neighbors()
{
	// neighbor index: 0 = i+1 neighbor, 1 = i-1 neighbor, 2 = j+1 neighbor, 3 = j-1 neighbor
	if (level != 0) die("assign_firstlevel_neighbors function must be run from grid level 0");
	int i,j;
	for (i=0; i < u_N; i++) {
		for (j=0; j < w_N; j++) {
			if (j < w_N-1)
				cell[i][j]->neighbor[2] = cell[i][j+1];
			else
				cell[i][j]->neighbor[2] = NULL;

			if (j > 0) 
				cell[i][j]->neighbor[3] = cell[i][j-1];
			else
				cell[i][j]->neighbor[3] = NULL;

			if (i > 0) {
				cell[i][j]->neighbor[1] = cell[i-1][j];
				if (i < u_N-1)
					cell[i][j]->neighbor[0] = cell[i+1][j];
				else
					cell[i][j]->neighbor[0] = NULL;
			} else {
				cell[i][j]->neighbor[1] = NULL;
				cell[i][j]->neighbor[0] = cell[i+1][j];
			}
		}
	}
}

void SourcePixelGrid::assign_neighborhood()
{
	// assign neighbors of this cell, then update neighbors of neighbors of this cell
	// neighbor index: 0 = i+1 neighbor, 1 = i-1 neighbor, 2 = j+1 neighbor, 3 = j-1 neighbor
	assign_level_neighbors(level);
	int l,k;
	for (l=0; l < 4; l++)
		if ((neighbor[l] != NULL) and (neighbor[l]->cell != NULL)) {
		for (k=level; k <= levels; k++) {
			neighbor[l]->assign_level_neighbors(k);
		}
	}
}

void SourcePixelGrid::assign_all_neighbors()
{
	if (level!=0) die("assign_all_neighbors should only be run from level 0");

	int k,i,j;
	for (k=1; k < levels; k++) {
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				cell[i][j]->assign_level_neighbors(k); // we've just created our grid, so we only need to go to level+1
			}
		}
	}
}

void SourcePixelGrid::test_neighbors() // for testing purposes, to make sure neighbors are assigned correctly
{
	int k,i,j;
	for (i=0; i < u_N; i++) {
		for (j=0; j < w_N; j++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->test_neighbors();
			else {
				for (k=0; k < 4; k++) {
					if (cell[i][j]->neighbor[k] == NULL)
						cout << "Level " << cell[i][j]->level << " cell (" << i << "," << j << ") neighbor " << k << ": none\n";
					else
						cout << "Level " << cell[i][j]->level << " cell (" << i << "," << j << ") neighbor " << k << ": level " << cell[i][j]->neighbor[k]->level << " (" << cell[i][j]->neighbor[k]->ii << "," << cell[i][j]->neighbor[k]->jj << ")\n";
				}
			}
		}
	}
}

void SourcePixelGrid::assign_level_neighbors(int neighbor_level)
{
	if (cell == NULL) return;
	int i,j;
	if (level < neighbor_level) {
		for (i=0; i < u_N; i++)
			for (j=0; j < w_N; j++)
				cell[i][j]->assign_level_neighbors(neighbor_level);
	} else {
		if (cell==NULL) die("cannot find neighbors if no grid has been set up");
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				if (cell[i][j]==NULL) die("a subcell has been erased");
				if (i < u_N-1)
					cell[i][j]->neighbor[0] = cell[i+1][j];
				else {
					if (neighbor[0] == NULL) cell[i][j]->neighbor[0] = NULL;
					else if (neighbor[0]->cell != NULL) {
						if (j >= neighbor[0]->w_N) cell[i][j]->neighbor[0] = neighbor[0]->cell[0][neighbor[0]->w_N-1]; // allows for possibility that neighbor cell may not be split by the same number of cells
						else cell[i][j]->neighbor[0] = neighbor[0]->cell[0][j];
					} else
						cell[i][j]->neighbor[0] = neighbor[0];
				}

				if (i > 0)
					cell[i][j]->neighbor[1] = cell[i-1][j];
				else {
					if (neighbor[1] == NULL) cell[i][j]->neighbor[1] = NULL;
					else if (neighbor[1]->cell != NULL) {
						if (j >= neighbor[1]->w_N) cell[i][j]->neighbor[1] = neighbor[1]->cell[neighbor[1]->u_N-1][neighbor[1]->w_N-1]; // allows for possibility that neighbor cell may not be split by the same number of cells

						else cell[i][j]->neighbor[1] = neighbor[1]->cell[neighbor[1]->u_N-1][j];
					} else
						cell[i][j]->neighbor[1] = neighbor[1];
				}

				if (j < w_N-1)
					cell[i][j]->neighbor[2] = cell[i][j+1];
				else {
					if (neighbor[2] == NULL) cell[i][j]->neighbor[2] = NULL;
					else if (neighbor[2]->cell != NULL) {
						if (i >= neighbor[2]->u_N) cell[i][j]->neighbor[2] = neighbor[2]->cell[neighbor[2]->u_N-1][0];
						else cell[i][j]->neighbor[2] = neighbor[2]->cell[i][0];
					} else
						cell[i][j]->neighbor[2] = neighbor[2];
				}

				if (j > 0)
					cell[i][j]->neighbor[3] = cell[i][j-1];
				else {
					if (neighbor[3] == NULL) cell[i][j]->neighbor[3] = NULL;
					else if (neighbor[3]->cell != NULL) {
						if (i >= neighbor[3]->u_N) cell[i][j]->neighbor[3] = neighbor[3]->cell[neighbor[3]->u_N-1][neighbor[3]->w_N-1];
						else cell[i][j]->neighbor[3] = neighbor[3]->cell[i][neighbor[3]->w_N-1];
					} else
						cell[i][j]->neighbor[3] = neighbor[3];
				}
			}
		}
	}
}

void SourcePixelGrid::split_cells(const int usplit, const int wsplit, const int& thread)
{
	if (level >= max_levels+1)
		die("maximum number of splittings has been reached (%i)", max_levels);
	if (cell != NULL)
		die("subcells should not already be present in split_cells routine");

	u_N = usplit;
	w_N = wsplit;
	int i,j;
	for (i=0; i <= u_N; i++) {
		for (j=0; j <= w_N; j++) {
			xvals_threads[thread][i][j][0] = ((corner_pt[0][0]*(w_N-j) + corner_pt[1][0]*j)*(u_N-i) + (corner_pt[2][0]*(w_N-j) + corner_pt[3][0]*j)*i)/(u_N*w_N);
			xvals_threads[thread][i][j][1] = ((corner_pt[0][1]*(w_N-j) + corner_pt[1][1]*j)*(u_N-i) + (corner_pt[2][1]*(w_N-j) + corner_pt[3][1]*j)*i)/(u_N*w_N);
		}
	}

	cell = new SourcePixelGrid**[u_N];
	for (i=0; i < u_N; i++)
	{
		cell[i] = new SourcePixelGrid*[w_N];
		for (j=0; j < w_N; j++) {
			cell[i][j] = new SourcePixelGrid(lens,xvals_threads[thread],i,j,level+1,this);
			cell[i][j]->total_magnification = 0;
			if (lens->n_image_prior) cell[i][j]->n_images = 0;
		}
	}
	if (level == maxlevs[thread]) {
		maxlevs[thread]++; // our subcells are at the max level, so splitting them increases the number of levels by 1
	}
	number_of_pixels += u_N*w_N - 1; // subtract one because we're not counting the parent cell as a source pixel
}

void SourcePixelGrid::unsplit()
{
	if (cell==NULL) return;
	surface_brightness = 0;
	int i,j;
	for (i=0; i < u_N; i++) {
		for (j=0; j < w_N; j++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->unsplit();
			surface_brightness += cell[i][j]->surface_brightness;
			delete cell[i][j];
		}
		delete[] cell[i];
	}
	delete[] cell;
	number_of_pixels -= (u_N*w_N - 1);
	cell = NULL;
	surface_brightness /= (u_N*w_N);
	u_N=1; w_N = 1;
}

ofstream SourcePixelGrid::xgrid;

void QLens::plot_source_pixel_grid(const char filename[])
{
	if (source_pixel_grid==NULL) { warn("No source surface brightness map has been generated"); return; }
	SourcePixelGrid::xgrid.open(filename, ifstream::out);
	source_pixel_grid->plot_corner_coordinates();
	SourcePixelGrid::xgrid.close();
}

void SourcePixelGrid::plot_corner_coordinates()
{
	if (level > 0) {
		xgrid << corner_pt[1][0] << " " << corner_pt[1][1] << endl;
		xgrid << corner_pt[3][0] << " " << corner_pt[3][1] << endl;
		xgrid << corner_pt[2][0] << " " << corner_pt[2][1] << endl;
		xgrid << corner_pt[0][0] << " " << corner_pt[0][1] << endl;
		xgrid << corner_pt[1][0] << " " << corner_pt[1][1] << endl;
		xgrid << endl;
	}

	if (cell != NULL)
		for (int i=0; i < u_N; i++)
			for (int j=0; j < w_N; j++)
				cell[i][j]->plot_corner_coordinates();
}

inline bool SourcePixelGrid::check_if_in_neighborhood(lensvector **input_corner_pts, bool& inside, const int& thread)
{
	if (trirec[thread].determine_if_in_neighborhood(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],*input_corner_pts[3],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1],inside)==true) return true;
	return false;
}

inline bool SourcePixelGrid::check_overlap(lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, const int& thread)
{
	if (twist_status==0) {
		if (trirec[thread].determine_if_overlap(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1])==true) return true;
		if (trirec[thread].determine_if_overlap(*input_corner_pts[1],*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1])==true) return true;
	} else if (twist_status==1) {
		if (trirec[thread].determine_if_overlap(*input_corner_pts[0],*input_corner_pts[2],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1])==true) return true;
		if (trirec[thread].determine_if_overlap(*input_corner_pts[1],*input_corner_pts[3],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1])==true) return true;
	} else {
		if (trirec[thread].determine_if_overlap(*input_corner_pts[0],*input_corner_pts[1],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1])==true) return true;
		if (trirec[thread].determine_if_overlap(*twist_pt,*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1])==true) return true;
	}
	return false;
}

inline double SourcePixelGrid::find_rectangle_overlap(lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, const int& thread, const int& i, const int& j)
{
	if (twist_status==0) {
		return (trirec[thread].find_overlap_area(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]) + trirec[thread].find_overlap_area(*input_corner_pts[1],*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
	} else if (twist_status==1) {
		return (trirec[thread].find_overlap_area(*input_corner_pts[0],*input_corner_pts[2],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]) + trirec[thread].find_overlap_area(*input_corner_pts[1],*input_corner_pts[3],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
	} else {
		return (trirec[thread].find_overlap_area(*input_corner_pts[0],*input_corner_pts[1],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]) + trirec[thread].find_overlap_area(*twist_pt,*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
	}
}

inline bool SourcePixelGrid::check_triangle1_overlap(lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, const int& thread)
{
	if (twist_status==0) {
		return trirec[thread].determine_if_overlap(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]);
	} else if (twist_status==1) {
		return trirec[thread].determine_if_overlap(*input_corner_pts[0],*input_corner_pts[2],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]);
	} else {
		return trirec[thread].determine_if_overlap(*input_corner_pts[0],*input_corner_pts[1],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]);
	}
}

inline bool SourcePixelGrid::check_triangle2_overlap(lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, const int& thread)
{
	if (twist_status==0) {
		return trirec[thread].determine_if_overlap(*input_corner_pts[1],*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]);
	} else if (twist_status==1) {
		return trirec[thread].determine_if_overlap(*input_corner_pts[1],*input_corner_pts[3],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]);
	} else {
		return trirec[thread].determine_if_overlap(*twist_pt,*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]);
	}
}

inline double SourcePixelGrid::find_triangle1_overlap(lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, const int& thread)
{
	if (twist_status==0) {
		return (trirec[thread].find_overlap_area(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
	} else if (twist_status==1) {
		return (trirec[thread].find_overlap_area(*input_corner_pts[0],*input_corner_pts[2],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
	} else {
		return (trirec[thread].find_overlap_area(*input_corner_pts[0],*input_corner_pts[1],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
	}
}

inline double SourcePixelGrid::find_triangle2_overlap(lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, const int& thread)
{
	if (twist_status==0) {
		return (trirec[thread].find_overlap_area(*input_corner_pts[1],*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
	} else if (twist_status==1) {
		return (trirec[thread].find_overlap_area(*input_corner_pts[1],*input_corner_pts[3],*twist_pt,corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
	} else {
		return (trirec[thread].find_overlap_area(*twist_pt,*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
	}
}

bool SourcePixelGrid::bisection_search_overlap(lensvector **input_corner_pts, const int& thread)
{
	int i, imid, jmid;
	bool inside;
	bool inside_corner[4];
	int n_inside;
	double xmin[4], xmax[4], ymin[4], ymax[4];
	int reduce_mid = 0;

	for (;;) {
		n_inside=0;
		for (i=0; i < 4; i++) inside_corner[i] = false;
		if (reduce_mid==0) {
			imid = (imax[thread] + imin[thread])/2;
			jmid = (jmax[thread] + jmin[thread])/2;
		} else if (reduce_mid==1) {
			imid = (imax[thread] + 2*imin[thread])/3;
			jmid = (jmax[thread] + 2*jmin[thread])/3;
		} else if (reduce_mid==2) {
			imid = (2*imax[thread] + imin[thread])/3;
			jmid = (2*jmax[thread] + jmin[thread])/3;
		} else if (reduce_mid==3) {
			imid = (imax[thread] + 2*imin[thread])/3;
			jmid = (2*jmax[thread] + jmin[thread])/3;
		} else if (reduce_mid==4) {
			imid = (2*imax[thread] + imin[thread])/3;
			jmid = (jmax[thread] + 2*jmin[thread])/3;
		}
		if ((imid==imin[thread]) or ((imid==imax[thread]))) break;
		if ((jmid==jmin[thread]) or ((jmid==jmax[thread]))) break;
		xmin[0] = cell[imin[thread]][jmin[thread]]->corner_pt[0][0];
		ymin[0] = cell[imin[thread]][jmin[thread]]->corner_pt[0][1];
		xmax[0] = cell[imid][jmid]->corner_pt[3][0];
		ymax[0] = cell[imid][jmid]->corner_pt[3][1];

		xmin[1] = cell[imin[thread]][jmid+1]->corner_pt[0][0];
		ymin[1] = cell[imin[thread]][jmid+1]->corner_pt[0][1];
		xmax[1] = cell[imid][jmax[thread]]->corner_pt[3][0];
		ymax[1] = cell[imid][jmax[thread]]->corner_pt[3][1];

		xmin[2] = cell[imid+1][jmin[thread]]->corner_pt[0][0];
		ymin[2] = cell[imid+1][jmin[thread]]->corner_pt[0][1];
		xmax[2] = cell[imax[thread]][jmid]->corner_pt[3][0];
		ymax[2] = cell[imax[thread]][jmid]->corner_pt[3][1];

		xmin[3] = cell[imid+1][jmid+1]->corner_pt[0][0];
		ymin[3] = cell[imid+1][jmid+1]->corner_pt[0][1];
		xmax[3] = cell[imax[thread]][jmax[thread]]->corner_pt[3][0];
		ymax[3] = cell[imax[thread]][jmax[thread]]->corner_pt[3][1];

		for (i=0; i < 4; i++) {
			inside = false;
			if (trirec[thread].determine_if_in_neighborhood(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],*input_corner_pts[3],xmin[i],xmax[i],ymin[i],ymax[i],inside)) {
				if (inside) inside_corner[i] = true;
				else if (trirec[thread].determine_if_overlap(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],xmin[i],xmax[i],ymin[i],ymax[i])) inside_corner[i] = true;
				else if (trirec[thread].determine_if_overlap(*input_corner_pts[1],*input_corner_pts[2],*input_corner_pts[3],xmin[i],xmax[i],ymin[i],ymax[i])) inside_corner[i] = true;
				if (inside_corner[i]) n_inside++;
			}
		}
		if (n_inside==0) return false;
		if (n_inside > 1) {
			if (reduce_mid>0) {
				if (reduce_mid < 4) { reduce_mid++; continue; }
				else break; // tried shifting the dividing lines to 1/3 & 2/3 positions, just in case the cell was straddling the middle, but still didn't contain the cell, so give up
			}
			else {
				reduce_mid = 1;
				continue;
			}
		} else if (reduce_mid>0) {
			reduce_mid = 0;
		}

		if (inside_corner[0]) { imax[thread]=imid; jmax[thread]=jmid; }
		else if (inside_corner[1]) { imax[thread]=imid; jmin[thread]=jmid; }
		else if (inside_corner[2]) { imin[thread]=imid; jmax[thread]=jmid; }
		else if (inside_corner[3]) { imin[thread]=imid; jmin[thread]=jmid; }
		if ((imax[thread] - imin[thread] <= 1) or (jmax[thread] - jmin[thread] <= 1)) break;
	}
	return true;
}

void SourcePixelGrid::calculate_pixel_magnifications()
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*(lens->group_comm), *(lens->mpi_group), &sub_comm);
#endif

	lens->total_srcgrid_overlap_area = 0; // Used to find the total coverage of the sourcegrid, which helps determine optimal source pixel size
	lens->high_sn_srcgrid_overlap_area = 0; // Used to find the total coverage of the sourcegrid, which helps determine optimal source pixel size

	int i,j,k,nsrc;
	double overlap_area, weighted_overlap, triangle1_overlap, triangle2_overlap, triangle1_weight, triangle2_weight;
	bool inside;
	clear_subgrids();
	int ntot_src = u_N*w_N;
	double *area_matrix, *mag_matrix;
	double *high_sn_area_matrix;
	mag_matrix = new double[ntot_src];
	area_matrix = new double[ntot_src];
	high_sn_area_matrix = new double[ntot_src];
	for (i=0; i < ntot_src; i++) {
		area_matrix[i] = 0;
		high_sn_area_matrix[i] = 0;
		mag_matrix[i] = 0;
	}
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			cell[i][j]->overlap_pixel_n.clear();
		}
	}

#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	double xstep, ystep;
	xstep = (srcgrid_xmax-srcgrid_xmin)/u_N;
	ystep = (srcgrid_ymax-srcgrid_ymin)/w_N;
	int src_raytrace_i, src_raytrace_j;
	int img_i, img_j;

	long int ntot_cells = image_pixel_grid->ntot_cells;

	/*
	long int ntot = 0;
	for (i=0; i < image_pixel_grid->x_N; i++) {
		for (j=0; j < image_pixel_grid->y_N; j++) {
			if (lens->image_pixel_data->in_mask[i][j]) ntot++;
			//if (lens->image_pixel_data->extended_mask[i][j]) ntot++;
		}
	}
	int *mask_i = new int[ntot];
	int *mask_j = new int[ntot];
	int n_cell=0;
	// you shouldn't have to calculate this again...this was already calculated in redo_lensing_calculations. Save mask_i arrays?
	for (j=0; j < image_pixel_grid->y_N; j++) {
		for (i=0; i < image_pixel_grid->x_N; i++) {
			if (lens->image_pixel_data->in_mask[i][j]) {
			//if (lens->image_pixel_data->extended_mask[i][j]) {
				mask_i[n_cell] = i;
				mask_j[n_cell] = j;
				n_cell++;
			}
		}
	}
	cout << "NTOT: " << ntot << " " << ntot_cells << endl;
	*/

	int *overlap_matrix_row_nn = new int[ntot_cells];
	vector<double> *overlap_matrix_rows = new vector<double>[ntot_cells];
	vector<int> *overlap_matrix_index_rows = new vector<int>[ntot_cells];
	vector<double> *overlap_area_matrix_rows;
	overlap_area_matrix_rows = new vector<double>[ntot_cells];

	int mpi_chunk, mpi_start, mpi_end;
	mpi_chunk = ntot_cells / lens->group_np;
	mpi_start = lens->group_id*mpi_chunk;
	if (lens->group_id == lens->group_np-1) mpi_chunk += (ntot_cells % lens->group_np); // assign the remainder elements to the last mpi process
	mpi_end = mpi_start + mpi_chunk;

	int overlap_matrix_nn;
	int overlap_matrix_nn_part=0;
	#pragma omp parallel
	{
		int n, img_i, img_j;
		bool inside;
		int thread;
		int corner_raytrace_i;
		int corner_raytrace_j;
		int min_i, max_i, min_j, max_j;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
		#pragma omp for private(i,j,nsrc,overlap_area,weighted_overlap,triangle1_overlap,triangle2_overlap,triangle1_weight,triangle2_weight,inside) schedule(dynamic) reduction(+:overlap_matrix_nn_part)
		for (n=mpi_start; n < mpi_end; n++)
		{
			overlap_matrix_row_nn[n] = 0;
			//img_j = n / image_pixel_grid->x_N;
			//img_i = n % image_pixel_grid->x_N;
			img_j = image_pixel_grid->masked_pixels_j[n];
			img_i = image_pixel_grid->masked_pixels_i[n];

			corners_threads[thread][0] = &image_pixel_grid->corner_sourcepts[img_i][img_j];
			corners_threads[thread][1] = &image_pixel_grid->corner_sourcepts[img_i][img_j+1];
			corners_threads[thread][2] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j];
			corners_threads[thread][3] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j+1];
			//for (int l=0; l < 4; l++) if ((*corners_threads[thread][l])[0]==-5000) {
				//cout << "WHOOPS! " << l << " " << img_i << " " << img_j << " " << endl;
				//cout << "checking corner 0: " << image_pixel_grid->corner_sourcepts[img_i][img_j][0] << " " << image_pixel_grid->corner_sourcepts[img_i][img_j][1] << endl;
				//cout << "checking corner 1: " << image_pixel_grid->corner_sourcepts[img_i][img_j+1][0] << " " << image_pixel_grid->corner_sourcepts[img_i][img_j+1][1] << endl;
				//cout << "checking corner 2: " << image_pixel_grid->corner_sourcepts[img_i+1][img_j][0] << " " << image_pixel_grid->corner_sourcepts[img_i+1][img_j][1] << endl;
				//cout << "checking corner 3: " << image_pixel_grid->corner_sourcepts[img_i+1][img_j+1][0] << " " << image_pixel_grid->corner_sourcepts[img_i+1][img_j+1][1] << endl;
				//cout << "checking center: " << image_pixel_grid->center_sourcepts[img_i][img_j][0] << " " << image_pixel_grid->center_sourcepts[img_i][img_j][1] << endl;
				////die("OOPSY DOOPSIES!");
			//}
			twistpts_threads[thread] = &image_pixel_grid->twist_pts[img_i][img_j];
			twist_status_threads[thread] = &image_pixel_grid->twist_status[img_i][img_j];

			min_i = (int) (((*corners_threads[thread][0])[0] - srcgrid_xmin) / xstep);
			min_j = (int) (((*corners_threads[thread][0])[1] - srcgrid_ymin) / ystep);
			max_i = min_i;
			max_j = min_j;
			for (i=1; i < 4; i++) {
				corner_raytrace_i = (int) (((*corners_threads[thread][i])[0] - srcgrid_xmin) / xstep);
				corner_raytrace_j = (int) (((*corners_threads[thread][i])[1] - srcgrid_ymin) / ystep);
				if (corner_raytrace_i < min_i) min_i = corner_raytrace_i;
				if (corner_raytrace_i > max_i) max_i = corner_raytrace_i;
				if (corner_raytrace_j < min_j) min_j = corner_raytrace_j;
				if (corner_raytrace_j > max_j) max_j = corner_raytrace_j;
			}
			if ((min_i < 0) or (min_i >= u_N)) continue;
			if ((min_j < 0) or (min_j >= w_N)) continue;
			if ((max_i < 0) or (max_i >= u_N)) continue;
			if ((max_j < 0) or (max_j >= w_N)) continue;

			for (j=min_j; j <= max_j; j++) {
				for (i=min_i; i <= max_i; i++) {
					nsrc = j*u_N + i;
					if (cell[i][j]->check_if_in_neighborhood(corners_threads[thread],inside,thread)) {
						if (inside) {
							triangle1_overlap = cell[i][j]->find_triangle1_overlap(corners_threads[thread],twistpts_threads[thread],*twist_status_threads[thread],thread);
							triangle2_overlap = cell[i][j]->find_triangle2_overlap(corners_threads[thread],twistpts_threads[thread],*twist_status_threads[thread],thread);
							triangle1_weight = triangle1_overlap / image_pixel_grid->source_plane_triangle1_area[img_i][img_j];
							triangle2_weight = triangle2_overlap / image_pixel_grid->source_plane_triangle2_area[img_i][img_j];
							//cout << triangle1_overlap << " " << triangle2_overlap << " " << image_pixel_grid->source_plane_triangle1_area[img_i][img_j] << endl;
						} else {
							if (cell[i][j]->check_triangle1_overlap(corners_threads[thread],twistpts_threads[thread],*twist_status_threads[thread],thread)) {
								triangle1_overlap = cell[i][j]->find_triangle1_overlap(corners_threads[thread],twistpts_threads[thread],*twist_status_threads[thread],thread);
								triangle1_weight = triangle1_overlap / image_pixel_grid->source_plane_triangle1_area[img_i][img_j];
							} else {
								triangle1_overlap = 0;
								triangle1_weight = 0;
							}
							if (cell[i][j]->check_triangle2_overlap(corners_threads[thread],twistpts_threads[thread],*twist_status_threads[thread],thread)) {
								triangle2_overlap = cell[i][j]->find_triangle2_overlap(corners_threads[thread],twistpts_threads[thread],*twist_status_threads[thread],thread);
								triangle2_weight = triangle2_overlap / image_pixel_grid->source_plane_triangle2_area[img_i][img_j];
							} else {
								triangle2_overlap = 0;
								triangle2_weight = 0;
							}
						}
						if ((triangle1_overlap != 0) or (triangle2_overlap != 0)) {
							weighted_overlap = triangle1_weight + triangle2_weight;
							//cout << "WEIGHT: " << weighted_overlap << endl;
							overlap_matrix_rows[n].push_back(weighted_overlap);
							overlap_matrix_index_rows[n].push_back(nsrc);
							overlap_matrix_row_nn[n]++;

							overlap_area = triangle1_overlap + triangle2_overlap;
							if ((image_pixel_grid->fit_to_data == NULL) or (!image_pixel_grid->fit_to_data[img_i][img_j])) overlap_area = 0;
							overlap_area_matrix_rows[n].push_back(overlap_area);
						}
					}
				}
			}
			overlap_matrix_nn_part += overlap_matrix_row_nn[n];
		}
	}

#ifdef USE_MPI
	MPI_Allreduce(&overlap_matrix_nn_part, &overlap_matrix_nn, 1, MPI_INT, MPI_SUM, sub_comm);
#else
	overlap_matrix_nn = overlap_matrix_nn_part;
#endif

	double *overlap_matrix = new double[overlap_matrix_nn];
	int *overlap_matrix_index = new int[overlap_matrix_nn];
	int *image_pixel_location_overlap = new int[ntot_cells+1];
	double *overlap_area_matrix;
	overlap_area_matrix = new double[overlap_matrix_nn];

#ifdef USE_MPI
	int id, chunk, start, end, length;
	for (id=0; id < lens->group_np; id++) {
		chunk = ntot_cells / lens->group_np;
		start = id*chunk;
		if (id == lens->group_np-1) chunk += (ntot_cells % lens->group_np); // assign the remainder elements to the last mpi process
		MPI_Bcast(overlap_matrix_row_nn + start,chunk,MPI_INT,id,sub_comm);
	}
#endif

	image_pixel_location_overlap[0] = 0;
	int n,l;
	for (n=0; n < ntot_cells; n++) {
		image_pixel_location_overlap[n+1] = image_pixel_location_overlap[n] + overlap_matrix_row_nn[n];
	}

	int indx;
	for (n=mpi_start; n < mpi_end; n++) {
		indx = image_pixel_location_overlap[n];
		for (j=0; j < overlap_matrix_row_nn[n]; j++) {
			overlap_matrix[indx+j] = overlap_matrix_rows[n][j];
			overlap_matrix_index[indx+j] = overlap_matrix_index_rows[n][j];
			overlap_area_matrix[indx+j] = overlap_area_matrix_rows[n][j];
		}
	}

#ifdef USE_MPI
	for (id=0; id < lens->group_np; id++) {
		chunk = ntot_cells / lens->group_np;
		start = id*chunk;
		if (id == lens->group_np-1) chunk += (ntot_cells % lens->group_np); // assign the remainder elements to the last mpi process
		end = start + chunk;
		length = image_pixel_location_overlap[end] - image_pixel_location_overlap[start];
		MPI_Bcast(overlap_matrix + image_pixel_location_overlap[start],length,MPI_DOUBLE,id,sub_comm);
		MPI_Bcast(overlap_matrix_index + image_pixel_location_overlap[start],length,MPI_INT,id,sub_comm);
		MPI_Bcast(overlap_area_matrix + image_pixel_location_overlap[start],length,MPI_DOUBLE,id,sub_comm);
	}
	MPI_Comm_free(&sub_comm);
#endif

	for (n=0; n < ntot_cells; n++) {
		//img_j = n / image_pixel_grid->x_N;
		//img_i = n % image_pixel_grid->x_N;
		img_j = image_pixel_grid->masked_pixels_j[n];
		img_i = image_pixel_grid->masked_pixels_i[n];
		for (l=image_pixel_location_overlap[n]; l < image_pixel_location_overlap[n+1]; l++) {
			nsrc = overlap_matrix_index[l];
			j = nsrc / u_N;
			i = nsrc % u_N;
			mag_matrix[nsrc] += overlap_matrix[l];
			area_matrix[nsrc] += overlap_area_matrix[l];
			if ((image_pixel_grid->fit_to_data != NULL) and (lens->image_pixel_data->high_sn_pixel[img_i][img_j])) high_sn_area_matrix[nsrc] += overlap_area_matrix[l];
			cell[i][j]->overlap_pixel_n.push_back(n);
			if ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[img_i][img_j]==true)) cell[i][j]->maps_to_image_window = true;
		}
	}

#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for finding source cell magnifications: " << wtime << endl;
	}
#endif

	for (nsrc=0; nsrc < ntot_src; nsrc++) {
		j = nsrc / u_N;
		i = nsrc % u_N;
		cell[i][j]->total_magnification = mag_matrix[nsrc] * image_pixel_grid->triangle_area / cell[i][j]->cell_area;
		cell[i][j]->avg_image_pixels_mapped = cell[i][j]->total_magnification * cell[i][j]->cell_area / image_pixel_grid->pixel_area;
		if (lens->n_image_prior) cell[i][j]->n_images = area_matrix[nsrc] / cell[i][j]->cell_area;

		if (area_matrix[nsrc] > cell[i][j]->cell_area) lens->total_srcgrid_overlap_area += cell[i][j]->cell_area;
		else lens->total_srcgrid_overlap_area += area_matrix[nsrc];
		if (image_pixel_grid->fit_to_data != NULL) {
			if (high_sn_area_matrix[nsrc] > cell[i][j]->cell_area) lens->high_sn_srcgrid_overlap_area += cell[i][j]->cell_area;
			else lens->high_sn_srcgrid_overlap_area += high_sn_area_matrix[nsrc];
		}
		if (cell[i][j]->total_magnification*0.0) warn("Nonsensical source cell magnification (mag=%g",cell[i][j]->total_magnification);
	}

	delete[] overlap_matrix;
	delete[] overlap_matrix_index;
	delete[] image_pixel_location_overlap;
	delete[] overlap_matrix_rows;
	delete[] overlap_matrix_index_rows;
	delete[] overlap_matrix_row_nn;
	delete[] mag_matrix;
	delete[] overlap_area_matrix;
	delete[] overlap_area_matrix_rows;
	delete[] area_matrix;
	delete[] high_sn_area_matrix;
}

double SourcePixelGrid::get_lowest_mag_sourcept(double &xsrc, double &ysrc)
{
	double lowest_mag = 1e30;
	int i, j, i_lowest_mag, j_lowest_mag;
	for (i=0; i < u_N; i++) {
		for (j=0; j < w_N; j++) {
			if (cell[i][j]->maps_to_image_window) {
				if (cell[i][j]->total_magnification < lowest_mag) {
					lowest_mag = cell[i][j]->total_magnification;
					i_lowest_mag = i;
					j_lowest_mag = j;
				}
			}
		}
	}
	xsrc = cell[i_lowest_mag][j_lowest_mag]->center_pt[0];
	ysrc = cell[i_lowest_mag][j_lowest_mag]->center_pt[1];
	return lowest_mag;
}

void SourcePixelGrid::get_highest_mag_sourcept(double &xsrc, double &ysrc)
{
	double highest_mag = -1e30;
	int i, j, i_highest_mag, j_highest_mag;
	for (i=0; i < u_N; i++) {
		for (j=0; j < w_N; j++) {
			if (cell[i][j]->maps_to_image_window) {
				if (cell[i][j]->total_magnification > highest_mag) {
					highest_mag = cell[i][j]->total_magnification;
					i_highest_mag = i;
					j_highest_mag = j;
				}
			}
		}
	}
	xsrc = cell[i_highest_mag][j_highest_mag]->center_pt[0];
	ysrc = cell[i_highest_mag][j_highest_mag]->center_pt[1];
}

void SourcePixelGrid::adaptive_subgrid()
{
	calculate_pixel_magnifications();
#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	int i, prev_levels;
	for (i=0; i < max_levels-1; i++) {
		prev_levels = levels;
		split_subcells_firstlevel(i);
		if (prev_levels==levels) break; // no splitting occurred, so no need to attempt further subgridding
	}
	assign_all_neighbors();

#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for adaptive grid splitting: " << wtime << endl;
	}
#endif
}

void SourcePixelGrid::split_subcells_firstlevel(const int splitlevel)
{
	if (level >= max_levels+1)
		die("maximum number of splittings has been reached (%i)", max_levels);

	int ntot = u_N*w_N;
	int i,j,n;
	if (splitlevel > level) {
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			maxlevs[thread] = levels;
			#pragma omp for private(i,j,n) schedule(dynamic)
			for (n=0; n < ntot; n++) {
				j = n / u_N;
				i = n % u_N;
				if (cell[i][j]->cell != NULL) cell[i][j]->split_subcells(splitlevel,thread);
			}
		}
		for (i=0; i < nthreads; i++) if (maxlevs[i] > levels) levels = maxlevs[i];
	} else {
		int k,l,m;
		double overlap_area, weighted_overlap, triangle1_overlap, triangle2_overlap, triangle1_weight, triangle2_weight;
		SourcePixelGrid *subcell;
		bool subgrid;
		#pragma omp parallel
		{
			int nn, img_i, img_j;
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			maxlevs[thread] = levels;
			double xstep, ystep;
			xstep = (srcgrid_xmax-srcgrid_xmin)/u_N/2.0;
			ystep = (srcgrid_ymax-srcgrid_ymin)/w_N/2.0;
			int min_i,max_i,min_j,max_j;
			int corner_raytrace_i, corner_raytrace_j;
			int ii,lmin,lmax,mmin,mmax;

			#pragma omp for private(i,j,n,k,l,m,overlap_area,weighted_overlap,triangle1_overlap,triangle2_overlap,triangle1_weight,triangle2_weight,subgrid,subcell) schedule(dynamic)
			for (n=0; n < ntot; n++) {
				j = n / u_N;
				i = n % u_N;
				subgrid = false;
				if ((cell[i][j]->total_magnification*cell[i][j]->cell_area/(lens->base_srcpixel_imgpixel_ratio*image_pixel_grid->pixel_area)) > lens->pixel_magnification_threshold) subgrid = true;
				if (subgrid) {
					//cout << "SPLITTING(FIRST): level=" << cell[i][j]->level << ", mag=" << cell[i][j]->total_magnification << " fac=" << (cell[i][j]->cell_area/(lens->base_srcpixel_imgpixel_ratio*image_pixel_grid->pixel_area)) << endl;
					cell[i][j]->split_cells(2,2,thread);
					for (k=0; k < cell[i][j]->overlap_pixel_n.size(); k++) {
						nn = cell[i][j]->overlap_pixel_n[k];
						//img_j = nn / image_pixel_grid->x_N;
						//img_i = nn % image_pixel_grid->x_N;
						img_j = image_pixel_grid->masked_pixels_j[nn];
						img_i = image_pixel_grid->masked_pixels_i[nn];

						corners_threads[thread][0] = &image_pixel_grid->corner_sourcepts[img_i][img_j];
						corners_threads[thread][1] = &image_pixel_grid->corner_sourcepts[img_i][img_j+1];
						corners_threads[thread][2] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j];
						corners_threads[thread][3] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j+1];
						twistpts_threads[thread] = &image_pixel_grid->twist_pts[img_i][img_j];
						twist_status_threads[thread] = &image_pixel_grid->twist_status[img_i][img_j];

						min_i = (int) (((*corners_threads[thread][0])[0] - cell[i][j]->corner_pt[0][0]) / xstep);
						min_j = (int) (((*corners_threads[thread][0])[1] - cell[i][j]->corner_pt[0][1]) / ystep);
						max_i = min_i;
						max_j = min_j;
						for (ii=1; ii < 4; ii++) {
							corner_raytrace_i = (int) (((*corners_threads[thread][ii])[0] - cell[i][j]->corner_pt[0][0]) / xstep);
							corner_raytrace_j = (int) (((*corners_threads[thread][ii])[1] - cell[i][j]->corner_pt[0][1]) / ystep);
							if (corner_raytrace_i < min_i) min_i = corner_raytrace_i;
							if (corner_raytrace_i > max_i) max_i = corner_raytrace_i;
							if (corner_raytrace_j < min_j) min_j = corner_raytrace_j;
							if (corner_raytrace_j > max_j) max_j = corner_raytrace_j;
						}
						lmin=0;
						lmax=cell[i][j]->u_N-1;
						mmin=0;
						mmax=cell[i][j]->w_N-1;
						if ((min_i >= 0) and (min_i < cell[i][j]->u_N)) lmin = min_i;
						if ((max_i >= 0) and (max_i < cell[i][j]->u_N)) lmax = max_i;
						if ((min_j >= 0) and (min_j < cell[i][j]->w_N)) mmin = min_j;
						if ((max_j >= 0) and (max_j < cell[i][j]->w_N)) mmax = max_j;

						for (l=lmin; l <= lmax; l++) {
							for (m=mmin; m <= mmax; m++) {
								subcell = cell[i][j]->cell[l][m];
								triangle1_overlap = subcell->find_triangle1_overlap(corners_threads[thread],twistpts_threads[thread],*twist_status_threads[thread],thread);
								triangle2_overlap = subcell->find_triangle2_overlap(corners_threads[thread],twistpts_threads[thread],*twist_status_threads[thread],thread);
								triangle1_weight = triangle1_overlap / image_pixel_grid->source_plane_triangle1_area[img_i][img_j];
								triangle2_weight = triangle2_overlap / image_pixel_grid->source_plane_triangle2_area[img_i][img_j];
								weighted_overlap = triangle1_weight + triangle2_weight;
								if ((triangle2_weight*0.0 != 0.0)) {
									cout << "HMM (" << img_i << "," << img_j << ") " << triangle2_overlap << " " << image_pixel_grid->source_plane_triangle2_area[img_i][img_j] << endl;
									cout << "    .... imgpixel: " << image_pixel_grid->center_pts[img_i][img_j][0] << " " << image_pixel_grid->center_pts[img_i][img_j][1] << ", srcpixel: " << cell[i][j]->xcenter << " " << cell[i][j]->ycenter << endl;
								}

								subcell->total_magnification += weighted_overlap;
								//cout << "MAG: " << triangle1_overlap << " " << triangle2_overlap << " " << image_pixel_grid->source_plane_triangle1_area[img_i][img_j] << " " << image_pixel_grid->source_plane_triangle2_area[img_i][img_j] << endl;
								//cout << "MAG: " << subcell->total_magnification << " " << image_pixel_grid->triangle_area << " " << subcell->cell_area << endl;
								if ((weighted_overlap != 0) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[img_i][img_j]==true))) subcell->maps_to_image_window = true;
								subcell->overlap_pixel_n.push_back(nn);
								if (lens->n_image_prior) {
									overlap_area = triangle1_overlap + triangle2_overlap;
									subcell->n_images += overlap_area;
								}
							}
						}
						if (subcell->total_magnification*0.0 != 0.0) die("Nonsensical subcell magnification");
					}
					for (l=0; l < cell[i][j]->u_N; l++) {
						for (m=0; m < cell[i][j]->w_N; m++) {
							subcell = cell[i][j]->cell[l][m];
							subcell->total_magnification *= image_pixel_grid->triangle_area / subcell->cell_area;
							//cout << "subcell mag: " << subcell->total_magnification << endl;
							subcell->avg_image_pixels_mapped = subcell->total_magnification * subcell->cell_area / image_pixel_grid->pixel_area;
							if (lens->n_image_prior) subcell->n_images /= subcell->cell_area;
						}
					}
				}
			}
		}
		for (i=0; i < nthreads; i++) if (maxlevs[i] > levels) levels = maxlevs[i];
	}
}

void SourcePixelGrid::split_subcells(const int splitlevel, const int thread)
{
	if (level >= max_levels+1)
		die("maximum number of splittings has been reached (%i)", max_levels);

	int i,j;
	if (splitlevel > level) {
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				if (cell[i][j]->cell != NULL) cell[i][j]->split_subcells(splitlevel,thread);
			}
		}
	} else {
		double xstep, ystep;
		xstep = (corner_pt[2][0] - corner_pt[0][0])/u_N/2.0;
		ystep = (corner_pt[1][1] - corner_pt[0][1])/w_N/2.0;
		int min_i,max_i,min_j,max_j;
		int corner_raytrace_i, corner_raytrace_j;
		int ii,lmin,lmax,mmin,mmax;

		int k,l,m,nn,img_i,img_j;
		double overlap_area, weighted_overlap, triangle1_overlap, triangle2_overlap, triangle1_weight, triangle2_weight;
		SourcePixelGrid *subcell;
		bool subgrid;
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				subgrid = false;
				if ((cell[i][j]->total_magnification*cell[i][j]->cell_area/(lens->base_srcpixel_imgpixel_ratio*image_pixel_grid->pixel_area)) > lens->pixel_magnification_threshold) subgrid = true;

				if (subgrid) {
					//cout << "SPLITTING: level=" << cell[i][j]->level << ", mag=" << cell[i][j]->total_magnification << " fac=" << (cell[i][j]->cell_area/(lens->base_srcpixel_imgpixel_ratio*image_pixel_grid->pixel_area)) << endl;
					cell[i][j]->split_cells(2,2,thread);
					for (k=0; k < cell[i][j]->overlap_pixel_n.size(); k++) {
						nn = cell[i][j]->overlap_pixel_n[k];
						//img_j = nn / image_pixel_grid->x_N;
						//img_i = nn % image_pixel_grid->x_N;
						img_j = image_pixel_grid->masked_pixels_j[nn];
						img_i = image_pixel_grid->masked_pixels_i[nn];

						corners_threads[thread][0] = &image_pixel_grid->corner_sourcepts[img_i][img_j];
						corners_threads[thread][1] = &image_pixel_grid->corner_sourcepts[img_i][img_j+1];
						corners_threads[thread][2] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j];
						corners_threads[thread][3] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j+1];
						twistpts_threads[thread] = &image_pixel_grid->twist_pts[img_i][img_j];
						twist_status_threads[thread] = &image_pixel_grid->twist_status[img_i][img_j];

						min_i = (int) (((*corners_threads[thread][0])[0] - cell[i][j]->corner_pt[0][0]) / xstep);
						min_j = (int) (((*corners_threads[thread][0])[1] - cell[i][j]->corner_pt[0][1]) / ystep);
						max_i = min_i;
						max_j = min_j;
						for (ii=1; ii < 4; ii++) {
							corner_raytrace_i = (int) (((*corners_threads[thread][ii])[0] - cell[i][j]->corner_pt[0][0]) / xstep);
							corner_raytrace_j = (int) (((*corners_threads[thread][ii])[1] - cell[i][j]->corner_pt[0][1]) / ystep);
							if (corner_raytrace_i < min_i) min_i = corner_raytrace_i;
							if (corner_raytrace_i > max_i) max_i = corner_raytrace_i;
							if (corner_raytrace_j < min_j) min_j = corner_raytrace_j;
							if (corner_raytrace_j > max_j) max_j = corner_raytrace_j;
						}
						lmin=0;
						lmax=cell[i][j]->u_N-1;
						mmin=0;
						mmax=cell[i][j]->w_N-1;
						if ((min_i >= 0) and (min_i < cell[i][j]->u_N)) lmin = min_i;
						if ((max_i >= 0) and (max_i < cell[i][j]->u_N)) lmax = max_i;
						if ((min_j >= 0) and (min_j < cell[i][j]->w_N)) mmin = min_j;
						if ((max_j >= 0) and (max_j < cell[i][j]->w_N)) mmax = max_j;

						for (l=lmin; l <= lmax; l++) {
							for (m=mmin; m <= mmax; m++) {
								subcell = cell[i][j]->cell[l][m];
								triangle1_overlap = subcell->find_triangle1_overlap(corners_threads[thread],twistpts_threads[thread],*twist_status_threads[thread],thread);
								triangle2_overlap = subcell->find_triangle2_overlap(corners_threads[thread],twistpts_threads[thread],*twist_status_threads[thread],thread);
								triangle1_weight = triangle1_overlap / image_pixel_grid->source_plane_triangle1_area[img_i][img_j];
								triangle2_weight = triangle2_overlap / image_pixel_grid->source_plane_triangle2_area[img_i][img_j];
								weighted_overlap = triangle1_weight + triangle2_weight;

								subcell->total_magnification += weighted_overlap;
								subcell->overlap_pixel_n.push_back(nn);
								if ((weighted_overlap != 0) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[img_i][img_j]==true))) subcell->maps_to_image_window = true;
								if (lens->n_image_prior) {
									overlap_area = triangle1_overlap + triangle2_overlap;
									subcell->n_images += overlap_area;
								}
							}
						}
					}
					for (l=0; l < cell[i][j]->u_N; l++) {
						for (m=0; m < cell[i][j]->w_N; m++) {
							subcell = cell[i][j]->cell[l][m];
							subcell->total_magnification *= image_pixel_grid->triangle_area / subcell->cell_area;
							//cout << "subcell mag: " << subcell->total_magnification << endl;
							subcell->avg_image_pixels_mapped = subcell->total_magnification * subcell->cell_area / image_pixel_grid->pixel_area;
							if (lens->n_image_prior) subcell->n_images /= subcell->cell_area;
						}
					}
				}
			}
		}
	}
}

bool SourcePixelGrid::assign_source_mapping_flags_overlap(lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, vector<SourcePixelGrid*>& mapped_cartesian_srcpixels, const int& thread)
{
	imin[thread]=0; imax[thread]=u_N-1;
	jmin[thread]=0; jmax[thread]=w_N-1;
	if (bisection_search_overlap(input_corner_pts,thread)==false) return false;

	bool image_pixel_maps_to_source_grid = false;
	bool inside;
	int i,j;
	for (j=jmin[thread]; j <= jmax[thread]; j++) {
		for (i=imin[thread]; i <= imax[thread]; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->subcell_assign_source_mapping_flags_overlap(input_corner_pts,twist_pt,twist_status,mapped_cartesian_srcpixels,thread,image_pixel_maps_to_source_grid);
			else {
				if (!cell[i][j]->check_if_in_neighborhood(input_corner_pts,inside,thread)) continue;
				if ((inside) or (cell[i][j]->check_overlap(input_corner_pts,twist_pt,twist_status,thread))) {
					cell[i][j]->maps_to_image_pixel = true;
					mapped_cartesian_srcpixels.push_back(cell[i][j]);
					//if ((image_pixel_i==41) and (image_pixel_j==11)) cout << "mapped cell: " << cell[i][j]->center_pt[0] << " " << cell[i][j]->center_pt[1] << endl;
					if (!image_pixel_maps_to_source_grid) image_pixel_maps_to_source_grid = true;
				}
			}
		}
	}
	return image_pixel_maps_to_source_grid;
}

void SourcePixelGrid::subcell_assign_source_mapping_flags_overlap(lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, vector<SourcePixelGrid*>& mapped_cartesian_srcpixels, const int& thread, bool& image_pixel_maps_to_source_grid)
{
	bool inside;
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->subcell_assign_source_mapping_flags_overlap(input_corner_pts,twist_pt,twist_status,mapped_cartesian_srcpixels,thread,image_pixel_maps_to_source_grid);
			else {
				if (!cell[i][j]->check_if_in_neighborhood(input_corner_pts,inside,thread)) continue;
				if ((inside) or (cell[i][j]->check_overlap(input_corner_pts,twist_pt,twist_status,thread))) {
					cell[i][j]->maps_to_image_pixel = true;
					mapped_cartesian_srcpixels.push_back(cell[i][j]);
					if (!image_pixel_maps_to_source_grid) image_pixel_maps_to_source_grid = true;
				}
			}
		}
	}
}

void SourcePixelGrid::calculate_Lmatrix_overlap(const int &img_index, const int &image_pixel_i, const int &image_pixel_j, int& index, lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, const int& thread)
{
	double overlap, total_overlap=0;
	int i,j,k;
	int Lmatrix_index_initial = index;
	SourcePixelGrid *subcell;

	for (i=0; i < image_pixel_grid->mapped_cartesian_srcpixels[image_pixel_i][image_pixel_j].size(); i++) {
		subcell = image_pixel_grid->mapped_cartesian_srcpixels[image_pixel_i][image_pixel_j][i];
		lens->Lmatrix_index_rows[img_index].push_back(subcell->active_index);
		overlap = subcell->find_rectangle_overlap(input_corner_pts,twist_pt,twist_status,thread,image_pixel_i,image_pixel_j);
		lens->Lmatrix_rows[img_index].push_back(overlap);
		index++;
		total_overlap += overlap;
	}

	if (total_overlap==0) die("image pixel should have mapped to at least one source pixel");
	for (i=Lmatrix_index_initial; i < index; i++)
		lens->Lmatrix_rows[img_index][i] /= total_overlap;
}

double SourcePixelGrid::find_lensed_surface_brightness_overlap(lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, const int& thread)
{
	imin[thread]=0; imax[thread]=u_N-1;
	jmin[thread]=0; jmax[thread]=w_N-1;
	if (bisection_search_overlap(input_corner_pts,thread)==false) return false;

	double total_overlap = 0;
	double total_weighted_surface_brightness = 0;
	double overlap;
	int i,j;
	for (j=jmin[thread]; j <= jmax[thread]; j++) {
		for (i=imin[thread]; i <= imax[thread]; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->find_lensed_surface_brightness_subcell_overlap(input_corner_pts,twist_pt,twist_status,thread,overlap,total_overlap,total_weighted_surface_brightness);
			else {
				overlap = cell[i][j]->find_rectangle_overlap(input_corner_pts,twist_pt,twist_status,thread,0,0);
				total_overlap += overlap;
				total_weighted_surface_brightness += overlap*cell[i][j]->surface_brightness;
			}
		}
	}
	double lensed_surface_brightness;
	if (total_overlap==0) lensed_surface_brightness = 0;
	else lensed_surface_brightness = total_weighted_surface_brightness/total_overlap;
	return lensed_surface_brightness;
}

void SourcePixelGrid::find_lensed_surface_brightness_subcell_overlap(lensvector **input_corner_pts, lensvector *twist_pt, int& twist_status, const int& thread, double& overlap, double& total_overlap, double& total_weighted_surface_brightness)
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->find_lensed_surface_brightness_subcell_overlap(input_corner_pts,twist_pt,twist_status,thread,overlap,total_overlap,total_weighted_surface_brightness);
			else {
				overlap = cell[i][j]->find_rectangle_overlap(input_corner_pts,twist_pt,twist_status,thread,0,0);
				total_overlap += overlap;
				total_weighted_surface_brightness += overlap*cell[i][j]->surface_brightness;
			}
		}
	}
}

bool SourcePixelGrid::bisection_search_interpolate(lensvector &input_center_pt, const int& thread)
{
	int i, imid, jmid;
	bool inside;
	bool inside_corner[4];
	int n_inside;
	double xmin[4], xmax[4], ymin[4], ymax[4];

	for (;;) {
		n_inside=0;
		for (i=0; i < 4; i++) inside_corner[i] = false;
		imid = (imax[thread] + imin[thread])/2;
		jmid = (jmax[thread] + jmin[thread])/2;
		xmin[0] = cell[imin[thread]][jmin[thread]]->corner_pt[0][0];
		ymin[0] = cell[imin[thread]][jmin[thread]]->corner_pt[0][1];
		xmax[0] = cell[imid][jmid]->corner_pt[3][0];
		ymax[0] = cell[imid][jmid]->corner_pt[3][1];

		xmin[1] = cell[imin[thread]][jmid+1]->corner_pt[0][0];
		ymin[1] = cell[imin[thread]][jmid+1]->corner_pt[0][1];
		xmax[1] = cell[imid][jmax[thread]]->corner_pt[3][0];
		ymax[1] = cell[imid][jmax[thread]]->corner_pt[3][1];

		xmin[2] = cell[imid+1][jmin[thread]]->corner_pt[0][0];
		ymin[2] = cell[imid+1][jmin[thread]]->corner_pt[0][1];
		xmax[2] = cell[imax[thread]][jmid]->corner_pt[3][0];
		ymax[2] = cell[imax[thread]][jmid]->corner_pt[3][1];

		xmin[3] = cell[imid+1][jmid+1]->corner_pt[0][0];
		ymin[3] = cell[imid+1][jmid+1]->corner_pt[0][1];
		xmax[3] = cell[imax[thread]][jmax[thread]]->corner_pt[3][0];
		ymax[3] = cell[imax[thread]][jmax[thread]]->corner_pt[3][1];

		for (i=0; i < 4; i++) {
			if ((input_center_pt[0] >= xmin[i]) and (input_center_pt[0] < xmax[i]) and (input_center_pt[1] >= ymin[i]) and (input_center_pt[1] < ymax[i])) {
				inside_corner[i] = true;
				n_inside++;
			}
		}
		if (n_inside==0) return false;
		if (n_inside > 1) die("should not be inside more than one rectangle");
		else {
			if (inside_corner[0]) { imax[thread]=imid; jmax[thread]=jmid; }
			else if (inside_corner[1]) { imax[thread]=imid; jmin[thread]=jmid; }
			else if (inside_corner[2]) { imin[thread]=imid; jmax[thread]=jmid; }
			else if (inside_corner[3]) { imin[thread]=imid; jmin[thread]=jmid; }
		}
		if ((imax[thread] - imin[thread] <= 1) or (jmax[thread] - jmin[thread] <= 1)) break;
	}
	return true;
}

bool SourcePixelGrid::assign_source_mapping_flags_interpolate(lensvector &input_center_pt, vector<SourcePixelGrid*>& mapped_cartesian_srcpixels, const int& thread, const int& image_pixel_i, const int& image_pixel_j)
{
	bool image_pixel_maps_to_source_grid = false;
	// when splitting image pixels, there could be multiple entries in the Lmatrix array that belong to the same source pixel; you might save computational time if these can be consolidated (by adding them together). Try this out later
	imin[thread]=0; imax[thread]=u_N-1;
	jmin[thread]=0; jmax[thread]=w_N-1;
	if (bisection_search_interpolate(input_center_pt,thread)==true) {
		int i,j,side;
		SourcePixelGrid* cellptr;
		int oldsize = mapped_cartesian_srcpixels.size();
		for (j=jmin[thread]; j <= jmax[thread]; j++) {
			for (i=imin[thread]; i <= imax[thread]; i++) {
				if ((input_center_pt[0] >= cell[i][j]->corner_pt[0][0]) and (input_center_pt[0] < cell[i][j]->corner_pt[2][0]) and (input_center_pt[1] >= cell[i][j]->corner_pt[0][1]) and (input_center_pt[1] < cell[i][j]->corner_pt[3][1])) {
					if (cell[i][j]->cell != NULL) image_pixel_maps_to_source_grid = cell[i][j]->subcell_assign_source_mapping_flags_interpolate(input_center_pt,mapped_cartesian_srcpixels,thread);
					else {
						cell[i][j]->maps_to_image_pixel = true;
						mapped_cartesian_srcpixels.push_back(cell[i][j]);
						if (!image_pixel_maps_to_source_grid) image_pixel_maps_to_source_grid = true;
						if (((input_center_pt[0] > cell[i][j]->center_pt[0]) and (cell[i][j]->neighbor[0] != NULL)) or (cell[i][j]->neighbor[1] == NULL)) {
							if (cell[i][j]->neighbor[0]->cell != NULL) {
								side=0;
								cellptr = cell[i][j]->neighbor[0]->find_nearest_neighbor_cell(input_center_pt,side);
								cellptr->maps_to_image_pixel = true;
								mapped_cartesian_srcpixels.push_back(cellptr);
								//cout << "Adding to maps " << image_pixel_i << " " << image_pixel_j << endl;
							}
							else {
								cell[i][j]->neighbor[0]->maps_to_image_pixel = true;
								mapped_cartesian_srcpixels.push_back(cell[i][j]->neighbor[0]);
								//cout << "Adding to maps " << image_pixel_i << " " << image_pixel_j << endl;
							}
						} else {
							if (cell[i][j]->neighbor[1]->cell != NULL) {
								side=1;
								cellptr = cell[i][j]->neighbor[1]->find_nearest_neighbor_cell(input_center_pt,side);
								cellptr->maps_to_image_pixel = true;
								mapped_cartesian_srcpixels.push_back(cellptr);
								//cout << "Adding to maps " << image_pixel_i << " " << image_pixel_j << endl;
							}
							else {
								cell[i][j]->neighbor[1]->maps_to_image_pixel = true;
								mapped_cartesian_srcpixels.push_back(cell[i][j]->neighbor[1]);
								//cout << "Adding to maps " << image_pixel_i << " " << image_pixel_j << endl;
							}
						}
						if (((input_center_pt[1] > cell[i][j]->center_pt[1]) and (cell[i][j]->neighbor[2] != NULL)) or (cell[i][j]->neighbor[3] == NULL)) {
							if (cell[i][j]->neighbor[2]->cell != NULL) {
								side=2;
								cellptr = cell[i][j]->neighbor[2]->find_nearest_neighbor_cell(input_center_pt,side);
								cellptr->maps_to_image_pixel = true;
								mapped_cartesian_srcpixels.push_back(cellptr);
								//cout << "Adding to maps " << image_pixel_i << " " << image_pixel_j << endl;
							}
							else {
								cell[i][j]->neighbor[2]->maps_to_image_pixel = true;
								mapped_cartesian_srcpixels.push_back(cell[i][j]->neighbor[2]);
								//cout << "Adding to maps " << image_pixel_i << " " << image_pixel_j << endl;
							}
						} else {
							if (cell[i][j]->neighbor[3]->cell != NULL) {
								side=3;
								cellptr = cell[i][j]->neighbor[3]->find_nearest_neighbor_cell(input_center_pt,side);
								cellptr->maps_to_image_pixel = true;
								mapped_cartesian_srcpixels.push_back(cellptr);
								//cout << "Adding to maps " << image_pixel_i << " " << image_pixel_j << endl;
							}
							else {
								cell[i][j]->neighbor[3]->maps_to_image_pixel = true;
								mapped_cartesian_srcpixels.push_back(cell[i][j]->neighbor[3]);
								//cout << "Adding to maps " << image_pixel_i << " " << image_pixel_j << endl;
							}
						}
					}
					break;
				}
			}
		}
		if ((mapped_cartesian_srcpixels.size() - oldsize) != 3) die("Did not assign enough interpolation cells!");
	} else {
		mapped_cartesian_srcpixels.push_back(NULL);
		mapped_cartesian_srcpixels.push_back(NULL);
		mapped_cartesian_srcpixels.push_back(NULL);
	}
	//if ((image_pixel_i==34) and (image_pixel_j==1)) {
		//if (!image_pixel_maps_to_source_grid) cout << "subpixel didn't map!!!" << endl;
		//cout << "SIZE: " << mapped_cartesian_srcpixels.size() << endl;
		//for (int i=0; i < mapped_cartesian_srcpixels.size(); i++) cout << "cell " << i << ": " << mapped_cartesian_srcpixels[i]->active_index << endl;
	//}
	return image_pixel_maps_to_source_grid;
}

bool SourcePixelGrid::subcell_assign_source_mapping_flags_interpolate(lensvector &input_center_pt, vector<SourcePixelGrid*>& mapped_cartesian_srcpixels, const int& thread)
{
	bool image_pixel_maps_to_source_grid = false;
	int i,j,side;
	SourcePixelGrid* cellptr;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if ((input_center_pt[0] >= cell[i][j]->corner_pt[0][0]) and (input_center_pt[0] < cell[i][j]->corner_pt[2][0]) and (input_center_pt[1] >= cell[i][j]->corner_pt[0][1]) and (input_center_pt[1] < cell[i][j]->corner_pt[3][1])) {
				if (cell[i][j]->cell != NULL) image_pixel_maps_to_source_grid = cell[i][j]->subcell_assign_source_mapping_flags_interpolate(input_center_pt,mapped_cartesian_srcpixels,thread);
				else {
					cell[i][j]->maps_to_image_pixel = true;
					mapped_cartesian_srcpixels.push_back(cell[i][j]);
					if (!image_pixel_maps_to_source_grid) image_pixel_maps_to_source_grid = true;
					if (((input_center_pt[0] > cell[i][j]->center_pt[0]) and (cell[i][j]->neighbor[0] != NULL)) or (cell[i][j]->neighbor[1] == NULL)) {
						if (cell[i][j]->neighbor[0]->cell != NULL) {
							side=0;
							cellptr = cell[i][j]->neighbor[0]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_cartesian_srcpixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[0]->maps_to_image_pixel = true;
							mapped_cartesian_srcpixels.push_back(cell[i][j]->neighbor[0]);
						}
					} else {
						if (cell[i][j]->neighbor[1]->cell != NULL) {
							side=1;
							cellptr = cell[i][j]->neighbor[1]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_cartesian_srcpixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[1]->maps_to_image_pixel = true;
							mapped_cartesian_srcpixels.push_back(cell[i][j]->neighbor[1]);
						}
					}
					if (((input_center_pt[1] > cell[i][j]->center_pt[1]) and (cell[i][j]->neighbor[2] != NULL)) or (cell[i][j]->neighbor[3] == NULL)) {
						if (cell[i][j]->neighbor[2]->cell != NULL) {
							side=2;
							cellptr = cell[i][j]->neighbor[2]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_cartesian_srcpixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[2]->maps_to_image_pixel = true;
							mapped_cartesian_srcpixels.push_back(cell[i][j]->neighbor[2]);
						}
					} else {
						if (cell[i][j]->neighbor[3]->cell != NULL) {
							side=3;
							cellptr = cell[i][j]->neighbor[3]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_cartesian_srcpixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[3]->maps_to_image_pixel = true;
							mapped_cartesian_srcpixels.push_back(cell[i][j]->neighbor[3]);
						}
					}
				}
				break;
			}
		}
	}
	return image_pixel_maps_to_source_grid;
}

void SourcePixelGrid::calculate_Lmatrix_interpolate(const int img_index, const int image_pixel_i, const int image_pixel_j, int& index, lensvector &input_center_pt, const int& ii, const double weight, const int& thread)
{
	for (int i=0; i < 3; i++) {
		//cout << "What " << i << endl;
		//cout << "ii=" << ii << " trying index " << (3*ii+i) << endl;
		//cout << "imgpix: " << image_pixel_i << " " << image_pixel_j << endl;
		//cout << "size: " << image_pixel_grid->mapped_cartesian_srcpixels[image_pixel_i][image_pixel_j].size() << endl;
		if (image_pixel_grid->mapped_cartesian_srcpixels[image_pixel_i][image_pixel_j][3*ii+i] == NULL) return; // in this case, subpixel does not map to anything
		lens->Lmatrix_index_rows[img_index].push_back(image_pixel_grid->mapped_cartesian_srcpixels[image_pixel_i][image_pixel_j][3*ii+i]->active_index);
		//cout << "What? " << i << endl;
		interpolation_pts[i][thread] = &image_pixel_grid->mapped_cartesian_srcpixels[image_pixel_i][image_pixel_j][3*ii+i]->center_pt;
	}

	if (lens->interpolate_sb_3pt) {
		double d = ((*interpolation_pts[0][thread])[0]-(*interpolation_pts[1][thread])[0])*((*interpolation_pts[1][thread])[1]-(*interpolation_pts[2][thread])[1]) - ((*interpolation_pts[1][thread])[0]-(*interpolation_pts[2][thread])[0])*((*interpolation_pts[0][thread])[1]-(*interpolation_pts[1][thread])[1]);
		lens->Lmatrix_rows[img_index].push_back(weight*(input_center_pt[0]*((*interpolation_pts[1][thread])[1]-(*interpolation_pts[2][thread])[1]) + input_center_pt[1]*((*interpolation_pts[2][thread])[0]-(*interpolation_pts[1][thread])[0]) + (*interpolation_pts[1][thread])[0]*(*interpolation_pts[2][thread])[1] - (*interpolation_pts[1][thread])[1]*(*interpolation_pts[2][thread])[0])/d);
		lens->Lmatrix_rows[img_index].push_back(weight*(input_center_pt[0]*((*interpolation_pts[2][thread])[1]-(*interpolation_pts[0][thread])[1]) + input_center_pt[1]*((*interpolation_pts[0][thread])[0]-(*interpolation_pts[2][thread])[0]) + (*interpolation_pts[0][thread])[1]*(*interpolation_pts[2][thread])[0] - (*interpolation_pts[0][thread])[0]*(*interpolation_pts[2][thread])[1])/d);
		lens->Lmatrix_rows[img_index].push_back(weight*(input_center_pt[0]*((*interpolation_pts[0][thread])[1]-(*interpolation_pts[1][thread])[1]) + input_center_pt[1]*((*interpolation_pts[1][thread])[0]-(*interpolation_pts[0][thread])[0]) + (*interpolation_pts[0][thread])[0]*(*interpolation_pts[1][thread])[1] - (*interpolation_pts[0][thread])[1]*(*interpolation_pts[1][thread])[0])/d);
		if (d==0) warn("d is zero!!!");
	} else {
		lens->Lmatrix_rows[img_index].push_back(weight);
		lens->Lmatrix_rows[img_index].push_back(0);
		lens->Lmatrix_rows[img_index].push_back(0);
	}

	index += 3;
}

double SourcePixelGrid::find_lensed_surface_brightness_interpolate(lensvector &input_center_pt, const int& thread)
{
	lensvector *pts[3];
	double *sb[3];
	int indx=0;
	nearest_interpolation_cells[thread].found_containing_cell = false;
	for (int i=0; i < 3; i++) nearest_interpolation_cells[thread].pixel[i] = NULL;

	imin[thread]=0; imax[thread]=u_N-1;
	jmin[thread]=0; jmax[thread]=w_N-1;
	if (bisection_search_interpolate(input_center_pt,thread)==false) return false;

	bool image_pixel_maps_to_source_grid = false;
	int i,j,side;
	for (j=jmin[thread]; j <= jmax[thread]; j++) {
		for (i=imin[thread]; i <= imax[thread]; i++) {
			if ((input_center_pt[0] >= cell[i][j]->corner_pt[0][0]) and (input_center_pt[0] < cell[i][j]->corner_pt[2][0]) and (input_center_pt[1] >= cell[i][j]->corner_pt[0][1]) and (input_center_pt[1] < cell[i][j]->corner_pt[3][1])) {
				if (cell[i][j]->cell != NULL) cell[i][j]->find_interpolation_cells(input_center_pt,thread);
				else {
					nearest_interpolation_cells[thread].found_containing_cell = true;
					nearest_interpolation_cells[thread].pixel[0] = cell[i][j];
					if (((input_center_pt[0] > cell[i][j]->center_pt[0]) and (cell[i][j]->neighbor[0] != NULL)) or (cell[i][j]->neighbor[1] == NULL)) {
						if (cell[i][j]->neighbor[0]->cell != NULL) {
							side=0;
							nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[0]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[0];
					} else {
						if (cell[i][j]->neighbor[1]->cell != NULL) {
							side=1;
							nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[1]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[1];
					}
					if (((input_center_pt[1] > cell[i][j]->center_pt[1]) and (cell[i][j]->neighbor[2] != NULL)) or (cell[i][j]->neighbor[3] == NULL)) {
						if (cell[i][j]->neighbor[2]->cell != NULL) {
							side=2;
							nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[2]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[2];
					} else {
						if (cell[i][j]->neighbor[3]->cell != NULL) {
							side=3;
							nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[3]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[3];
					}
				}
				break;
			}
		}
	}

	for (i=0; i < 3; i++) {
		pts[i] = &nearest_interpolation_cells[thread].pixel[i]->center_pt;
		sb[i] = &nearest_interpolation_cells[thread].pixel[i]->surface_brightness;
	}

	if (nearest_interpolation_cells[thread].found_containing_cell==false) die("could not find containing cell");
	double d, total_sb = 0;
	d = ((*pts[0])[0]-(*pts[1])[0])*((*pts[1])[1]-(*pts[2])[1]) - ((*pts[1])[0]-(*pts[2])[0])*((*pts[0])[1]-(*pts[1])[1]);
	total_sb += (*sb[0])*(input_center_pt[0]*((*pts[1])[1]-(*pts[2])[1]) + input_center_pt[1]*((*pts[2])[0]-(*pts[1])[0]) + (*pts[1])[0]*(*pts[2])[1] - (*pts[1])[1]*(*pts[2])[0]);
	total_sb += (*sb[1])*(input_center_pt[0]*((*pts[2])[1]-(*pts[0])[1]) + input_center_pt[1]*((*pts[0])[0]-(*pts[2])[0]) + (*pts[0])[1]*(*pts[2])[0] - (*pts[0])[0]*(*pts[2])[1]);
	total_sb += (*sb[2])*(input_center_pt[0]*((*pts[0])[1]-(*pts[1])[1]) + input_center_pt[1]*((*pts[1])[0]-(*pts[0])[0]) + (*pts[0])[0]*(*pts[1])[1] - (*pts[0])[1]*(*pts[1])[0]);
	total_sb /= d;
	return total_sb;
}

void SourcePixelGrid::find_interpolation_cells(lensvector &input_center_pt, const int& thread)
{
	int i,j,side;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if ((input_center_pt[0] >= cell[i][j]->corner_pt[0][0]) and (input_center_pt[0] < cell[i][j]->corner_pt[2][0]) and (input_center_pt[1] >= cell[i][j]->corner_pt[0][1]) and (input_center_pt[1] < cell[i][j]->corner_pt[3][1])) {
				if (cell[i][j]->cell != NULL) cell[i][j]->find_interpolation_cells(input_center_pt,thread);
				else {
					nearest_interpolation_cells[thread].found_containing_cell = true;
					nearest_interpolation_cells[thread].pixel[0] = cell[i][j];
					if (((input_center_pt[0] > cell[i][j]->center_pt[0]) and (cell[i][j]->neighbor[0] != NULL)) or (cell[i][j]->neighbor[1] == NULL)) {
						if (cell[i][j]->neighbor[0]->cell != NULL) {
							side=0;
							nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[0]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[0];
					} else {
						if (cell[i][j]->neighbor[1]->cell != NULL) {
							side=1;
							nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[1]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[1];
					}
					if (((input_center_pt[1] > cell[i][j]->center_pt[1]) and (cell[i][j]->neighbor[2] != NULL)) or (cell[i][j]->neighbor[3] == NULL)) {
						if (cell[i][j]->neighbor[2]->cell != NULL) {
							side=2;
							nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[2]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[2];
					} else {
						if (cell[i][j]->neighbor[3]->cell != NULL) {
							side=3;
							nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[3]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[3];
					}
				}
				break;
			}
		}
	}
}

SourcePixelGrid* SourcePixelGrid::find_nearest_neighbor_cell(lensvector &input_center_pt, const int& side)
{
	int i,ncells;
	SourcePixelGrid **cells;
	if ((side==0) or (side==1)) ncells = w_N;
	else if ((side==2) or (side==3)) ncells = u_N;
	else die("side number cannot be larger than 3");
	cells = new SourcePixelGrid*[ncells];

	for (i=0; i < ncells; i++) {
		if (side==0) {
			if (cell[0][i]->cell != NULL) cells[i] = cell[0][i]->find_nearest_neighbor_cell(input_center_pt,side);
			else cells[i] = cell[0][i];
		} else if (side==1) {
			if (cell[u_N-1][i]->cell != NULL) cells[i] = cell[u_N-1][i]->find_nearest_neighbor_cell(input_center_pt,side);
			else cells[i] = cell[u_N-1][i];
		} else if (side==2) {
			if (cell[i][0]->cell != NULL) cells[i] = cell[i][0]->find_nearest_neighbor_cell(input_center_pt,side);
			else cells[i] = cell[i][0];
		} else if (side==3) {
			if (cell[i][w_N-1]->cell != NULL) cells[i] = cell[i][w_N-1]->find_nearest_neighbor_cell(input_center_pt,side);
			else cells[i] = cell[i][w_N-1];
		}
	}
	double sqr_distance, min_sqr_distance = 1e30;
	int i_min;
	for (i=0; i < ncells; i++) {
		sqr_distance = SQR(cells[i]->center_pt[0] - input_center_pt[0]) + SQR(cells[i]->center_pt[1] - input_center_pt[1]);
		if (sqr_distance < min_sqr_distance) {
			min_sqr_distance = sqr_distance;
			i_min = i;
		}
	}
	SourcePixelGrid *closest_cell = cells[i_min];
	delete[] cells;
	return closest_cell;
}

SourcePixelGrid* SourcePixelGrid::find_nearest_neighbor_cell(lensvector &input_center_pt, const int& side, const int tiebreaker_side)
{
	int i,ncells;
	SourcePixelGrid **cells;
	if ((side==0) or (side==1)) ncells = w_N;
	else if ((side==2) or (side==3)) ncells = u_N;
	else die("side number cannot be larger than 3");
	cells = new SourcePixelGrid*[ncells];
	double sqr_distance, min_sqr_distance = 1e30;
	SourcePixelGrid *closest_cell = NULL;
	int it=0, side_try=side;

	while ((closest_cell==NULL) and (it++ < 2))
	{
		for (i=0; i < ncells; i++) {
			if (side_try==0) {
				if (cell[0][i]->cell != NULL) cells[i] = cell[0][i]->find_nearest_neighbor_cell(input_center_pt,side);
				else cells[i] = cell[0][i];
			} else if (side_try==1) {
				if (cell[u_N-1][i]->cell != NULL) cells[i] = cell[u_N-1][i]->find_nearest_neighbor_cell(input_center_pt,side);
				else cells[i] = cell[u_N-1][i];
			} else if (side_try==2) {
				if (cell[i][0]->cell != NULL) cells[i] = cell[i][0]->find_nearest_neighbor_cell(input_center_pt,side);
				else cells[i] = cell[i][0];
			} else if (side_try==3) {
				if (cell[i][w_N-1]->cell != NULL) cells[i] = cell[i][w_N-1]->find_nearest_neighbor_cell(input_center_pt,side);
				else cells[i] = cell[i][w_N-1];
			}
		}
		for (i=0; i < ncells; i++) {
			sqr_distance = SQR(cells[i]->center_pt[0] - input_center_pt[0]) + SQR(cells[i]->center_pt[1] - input_center_pt[1]);
			if ((sqr_distance < min_sqr_distance) or ((sqr_distance==min_sqr_distance) and (i==tiebreaker_side))) {
				min_sqr_distance = sqr_distance;
				closest_cell = cells[i];
			}
		}
		if (closest_cell==NULL) {
			// in this case neither of the subcells in question mapped to the image plane, so we had better try again with the other two subcells.
			if (side_try==0) side_try = 1;
			else if (side_try==1) side_try = 0;
			else if (side_try==2) side_try = 3;
			else if (side_try==3) side_try = 2;
		}
	}
	delete[] cells;
	return closest_cell;
}

void SourcePixelGrid::find_nearest_two_cells(SourcePixelGrid* &cellptr1, SourcePixelGrid* &cellptr2, const int& side)
{
	if ((u_N != 2) or (w_N != 2)) die("cannot find nearest two cells unless splitting is two in either direction");
	if (side==0) {
		if (cell[0][0]->cell == NULL) cellptr1 = cell[0][0];
		else cellptr1 = cell[0][0]->find_corner_cell(0,1);
		if (cell[0][1]->cell == NULL) cellptr2 = cell[0][1];
		else cellptr2 = cell[0][1]->find_corner_cell(0,0);
	} else if (side==1) {
		if (cell[1][0]->cell == NULL) cellptr1 = cell[1][0];
		else cellptr1 = cell[1][0]->find_corner_cell(1,1);
		if (cell[1][1]->cell == NULL) cellptr2 = cell[1][1];
		else cellptr2 = cell[1][1]->find_corner_cell(1,0);
	} else if (side==2) {
		if (cell[0][0]->cell == NULL) cellptr1 = cell[0][0];
		else cellptr1 = cell[0][0]->find_corner_cell(1,0);
		if (cell[1][0]->cell == NULL) cellptr2 = cell[1][0];
		else cellptr2 = cell[1][0]->find_corner_cell(0,0);
	} else if (side==3) {
		if (cell[0][1]->cell == NULL) cellptr1 = cell[0][1];
		else cellptr1 = cell[0][1]->find_corner_cell(1,1);
		if (cell[1][1]->cell == NULL) cellptr2 = cell[1][1];
		else cellptr2 = cell[1][1]->find_corner_cell(0,1);
	}
}

SourcePixelGrid* SourcePixelGrid::find_corner_cell(const int i, const int j)
{
	SourcePixelGrid* cellptr = cell[i][j];
	while (cellptr->cell != NULL)
		cellptr = cellptr->cell[i][j];
	return cellptr;
}

void SourcePixelGrid::generate_gmatrices()
{
	int i,j,k,l;
	SourcePixelGrid *cellptr1, *cellptr2;
	double alpha, beta, dxfac;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->generate_gmatrices();
			else {
				if (cell[i][j]->active_pixel) {
					//dxfac = pow(1.3,-(cell[i][j]->level)); // seems like there's no real sensible reason to have a scaling factor here; delete this later
					dxfac = 1.0;
					for (k=0; k < 4; k++) {
						lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(1.0/dxfac);
						lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cell[i][j]->active_index);
						lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
						lens->gmatrix_nn[k]++;
						if (cell[i][j]->neighbor[k]) {
							if (cell[i][j]->neighbor[k]->cell != NULL) {
								cell[i][j]->neighbor[k]->find_nearest_two_cells(cellptr1,cellptr2,k);
								//cout << "cell 1: " << cellptr1->center_pt[0] << " " << cellptr1->center_pt[1] << endl;
								//cout << "cell 2: " << cellptr2->center_pt[0] << " " << cellptr2->center_pt[1] << endl;
								if ((cellptr1==NULL) or (cellptr2==NULL)) die("Hmm, not getting back two cells");
								if (k < 2) {
									// interpolating surface brightness along x-direction
									alpha = abs((cell[i][j]->center_pt[1] - cellptr1->center_pt[1]) / (cellptr2->center_pt[1] - cellptr1->center_pt[1]));
								} else {
									// interpolating surface brightness along y-direction
									alpha = abs((cell[i][j]->center_pt[0] - cellptr1->center_pt[0]) / (cellptr2->center_pt[0] - cellptr1->center_pt[0]));
								}
								beta = 1-alpha;
								if (cellptr1->active_pixel) {
									if (!cellptr2->active_pixel) beta=1; // just in case the other point is no good
									lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-beta/dxfac);
									lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr1->active_index);
									lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
									lens->gmatrix_nn[k]++;
								}
								if (cellptr2->active_pixel) {
									if (!cellptr1->active_pixel) alpha=1; // just in case the other point is no good
									lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-alpha/dxfac);
									lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr2->active_index);
									lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
									lens->gmatrix_nn[k]++;
								}
							}
							else if (cell[i][j]->neighbor[k]->active_pixel) {
								if (cell[i][j]->neighbor[k]->level==cell[i][j]->level) {
									lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-1.0/dxfac);
									lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cell[i][j]->neighbor[k]->active_index);
									lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
									lens->gmatrix_nn[k]++;
								} else {
									cellptr1 = cell[i][j]->neighbor[k];
									if (k < 2) {
										if (cellptr1->center_pt[1] > cell[i][j]->center_pt[1]) l=3;
										else l=2;
									} else {
										if (cellptr1->center_pt[0] > cell[i][j]->center_pt[0]) l=1;
										else l=0;
									}
									if ((cellptr1->neighbor[l]==NULL) or ((cellptr1->neighbor[l]->cell==NULL) and (!cellptr1->neighbor[l]->active_pixel))) {
										// There is no useful nearby neighbor to interpolate with, so just use the single neighbor pixel
										lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-1.0/dxfac);
										lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr1->active_index);
										lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
										lens->gmatrix_nn[k]++;
									} else {
										if (cellptr1->neighbor[l]->cell==NULL) cellptr2 = cellptr1->neighbor[l];
										else cellptr2 = cellptr1->neighbor[l]->find_nearest_neighbor_cell(cellptr1->center_pt,l,k%2); // the tiebreaker k%2 ensures that preference goes to cells that are closer to this cell in order to interpolate to find the gradient
										if (cellptr2==NULL) die("Subcell does not map to source pixel; regularization currently cannot handle unmapped subcells");
										if (k < 2) alpha = abs((cell[i][j]->center_pt[1] - cellptr1->center_pt[1]) / (cellptr2->center_pt[1] - cellptr1->center_pt[1]));
										else alpha = abs((cell[i][j]->center_pt[0] - cellptr1->center_pt[0]) / (cellptr2->center_pt[0] - cellptr1->center_pt[0]));
										beta = 1-alpha;
										//cout << alpha << " " << beta << " " << k << " " << l << " " << ii << " " << jj << " " << i << " " << j << endl;
										//cout << cell[i][j]->center_pt[0] << " " << cellptr1->center_pt[0] << " " << cellptr1->center_pt[1] << " " << cellptr2->center_pt[0] << " " << cellptr2->center_pt[1] << endl;
										if (cellptr1->active_pixel) {
											if (!cellptr2->active_pixel) beta=1; // just in case the other point is no good
											lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-beta/dxfac);
											lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr1->active_index);
											lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
											lens->gmatrix_nn[k]++;
										}
										if (cellptr2->active_pixel) {
											if (!cellptr1->active_pixel) alpha=1; // just in case the other point is no good
											lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-alpha/dxfac);
											lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr2->active_index);
											lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
											lens->gmatrix_nn[k]++;
										}

										//lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-beta);
										//lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr1->active_index);
										//lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-alpha);
										//lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr2->active_index);
										//lens->gmatrix_row_nn[k][cell[i][j]->active_index] += 2;
										//lens->gmatrix_nn[k] += 2;
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

void SourcePixelGrid::generate_hmatrices()
{
	int i,j,k,l,m,kmin,kmax;
	SourcePixelGrid *cellptr1, *cellptr2;
	double alpha, beta;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->generate_hmatrices();
			else {
				for (l=0; l < 2; l++) {
					if (cell[i][j]->active_pixel) {
						lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(-2);
						lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cell[i][j]->active_index);
						lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
						lens->hmatrix_nn[l]++;
						if (l==0) {
							kmin=0; kmax=1;
						} else {
							kmin=2; kmax=3;
						}
						for (k=kmin; k <= kmax; k++) {
							if (cell[i][j]->neighbor[k]) {
								if (cell[i][j]->neighbor[k]->cell != NULL) {
									cell[i][j]->neighbor[k]->find_nearest_two_cells(cellptr1,cellptr2,k);
									if ((cellptr1==NULL) or (cellptr2==NULL)) die("Hmm, not getting back two cells");
									if (k < 2) {
										// interpolating surface brightness along x-direction
										alpha = abs((cell[i][j]->center_pt[1] - cellptr1->center_pt[1]) / (cellptr2->center_pt[1] - cellptr1->center_pt[1]));
									} else {
										// interpolating surface brightness along y-direction
										alpha = abs((cell[i][j]->center_pt[0] - cellptr1->center_pt[0]) / (cellptr2->center_pt[0] - cellptr1->center_pt[0]));
									}
									beta = 1-alpha;
									if (!cellptr1->active_pixel) alpha=1;
									if (!cellptr2->active_pixel) beta=1;
									if (cellptr1->active_pixel) {
										lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(beta);
										lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cellptr1->active_index);
										lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
										lens->hmatrix_nn[l]++;
									}
									if (cellptr2->active_pixel) {
										lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(alpha);
										lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cellptr2->active_index);
										lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
										lens->hmatrix_nn[l]++;
									}

								}
								else if (cell[i][j]->neighbor[k]->active_pixel) {
									if (cell[i][j]->neighbor[k]->level==cell[i][j]->level) {
										lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(1);
										lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cell[i][j]->neighbor[k]->active_index);
										lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
										lens->hmatrix_nn[l]++;
									} else {
										cellptr1 = cell[i][j]->neighbor[k];
										if (k < 2) {
											if (cellptr1->center_pt[1] > cell[i][j]->center_pt[1]) m=3;
											else m=2;
										} else {
											if (cellptr1->center_pt[0] > cell[i][j]->center_pt[0]) m=1;
											else m=0;
										}
										if ((cellptr1->neighbor[m]==NULL) or ((cellptr1->neighbor[m]->cell==NULL) and (!cellptr1->neighbor[m]->active_pixel))) {
											// There is no useful nearby neighbor to interpolate with, so just use the single neighbor pixel
											lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(1);
											lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cellptr1->active_index);
											lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
											lens->hmatrix_nn[l]++;
										} else {
											if (cellptr1->neighbor[m]->cell==NULL) cellptr2 = cellptr1->neighbor[m];
											else cellptr2 = cellptr1->neighbor[m]->find_nearest_neighbor_cell(cellptr1->center_pt,m,k%2); // the tiebreaker k%2 ensures that preference goes to cells that are closer to this cell in order to interpolate to find the curvature
											if (cellptr2==NULL) die("Subcell does not map to source pixel; regularization currently cannot handle unmapped subcells");
											if (k < 2) alpha = abs((cell[i][j]->center_pt[1] - cellptr1->center_pt[1]) / (cellptr2->center_pt[1] - cellptr1->center_pt[1]));
											else alpha = abs((cell[i][j]->center_pt[0] - cellptr1->center_pt[0]) / (cellptr2->center_pt[0] - cellptr1->center_pt[0]));
											beta = 1-alpha;
											//cout << alpha << " " << beta << " " << k << " " << m << " " << ii << " " << jj << " " << i << " " << j << endl;
											//cout << cell[i][j]->center_pt[0] << " " << cellptr1->center_pt[0] << " " << cellptr1->center_pt[1] << " " << cellptr2->center_pt[0] << " " << cellptr2->center_pt[1] << endl;
											if (!cellptr1->active_pixel) alpha=1;
											if (!cellptr2->active_pixel) beta=1;
											if (cellptr1->active_pixel) {
												lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(beta);
												lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cellptr1->active_index);
												lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
												lens->hmatrix_nn[l]++;
											}
											if (cellptr2->active_pixel) {
												lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(alpha);
												lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cellptr2->active_index);
												lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
												lens->hmatrix_nn[l]++;
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

void QLens::generate_Rmatrix_from_hmatrices()
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	int i,j,k,l,m,n,indx;

	vector<int> *jvals[2];
	vector<int> *lvals[2];
	for (i=0; i < 2; i++) {
		jvals[i] = new vector<int>[source_npixels];
		lvals[i] = new vector<int>[source_npixels];
	}

	Rmatrix_diag_temp = new double[source_npixels];
	Rmatrix_rows = new vector<double>[source_npixels];
	Rmatrix_index_rows = new vector<int>[source_npixels];
	Rmatrix_row_nn = new int[source_npixels];
	Rmatrix_nn = 0;
	int Rmatrix_nn_part = 0;
	for (j=0; j < source_npixels; j++) {
		Rmatrix_diag_temp[j] = 0;
		Rmatrix_row_nn[j] = 0;
	}

	bool new_entry;
	int src_index1, src_index2, col_index, col_i;
	double tmp, element;

	for (k=0; k < 2; k++) {
		hmatrix_rows[k] = new vector<double>[source_npixels];
		hmatrix_index_rows[k] = new vector<int>[source_npixels];
		hmatrix_row_nn[k] = new int[source_npixels];
		hmatrix_nn[k] = 0;
		for (j=0; j < source_npixels; j++) {
			hmatrix_row_nn[k][j] = 0;
		}
	}
	if (source_fit_mode==Delaunay_Source) delaunay_srcgrid->generate_hmatrices();
	else if (source_fit_mode==Cartesian_Source) source_pixel_grid->generate_hmatrices();
	else die("hmatrix not supported for sources other than Delaunay or Cartesian");

	for (k=0; k < 2; k++) {
		hmatrix[k] = new double[hmatrix_nn[k]];
		hmatrix_index[k] = new int[hmatrix_nn[k]];
		hmatrix_row_index[k] = new int[source_npixels+1];

		hmatrix_row_index[k][0] = 0;
		for (i=0; i < source_npixels; i++)
			hmatrix_row_index[k][i+1] = hmatrix_row_index[k][i] + hmatrix_row_nn[k][i];
		if (hmatrix_row_index[k][source_npixels] != hmatrix_nn[k]) die("the number of elements don't match up for hmatrix %i",k);

		for (i=0; i < source_npixels; i++) {
			indx = hmatrix_row_index[k][i];
			for (j=0; j < hmatrix_row_nn[k][i]; j++) {
				hmatrix[k][indx+j] = hmatrix_rows[k][i][j];
				hmatrix_index[k][indx+j] = hmatrix_index_rows[k][i][j];
			}
		}
		delete[] hmatrix_rows[k];
		delete[] hmatrix_index_rows[k];
		delete[] hmatrix_row_nn[k];

		for (i=0; i < source_npixels; i++) {
			for (j=hmatrix_row_index[k][i]; j < hmatrix_row_index[k][i+1]; j++) {
				for (l=j; l < hmatrix_row_index[k][i+1]; l++) {
					src_index1 = hmatrix_index[k][j];
					src_index2 = hmatrix_index[k][l];
					if (src_index1 > src_index2) {
						tmp=src_index1;
						src_index1=src_index2;
						src_index2=tmp;
						jvals[k][src_index1].push_back(l);
						lvals[k][src_index1].push_back(j);
					} else {
						jvals[k][src_index1].push_back(j);
						lvals[k][src_index1].push_back(l);
					}
				}
			}
		}
	}

	#pragma omp parallel for private(i,j,k,l,m,n,src_index1,src_index2,new_entry,col_index,col_i,element) schedule(static) reduction(+:Rmatrix_nn_part)
	for (src_index1=0; src_index1 < source_npixels; src_index1++) {
		for (k=0; k < 2; k++) {
			col_i=0;
			for (n=0; n < jvals[k][src_index1].size(); n++) {
				j = jvals[k][src_index1][n];
				l = lvals[k][src_index1][n];
				src_index2 = hmatrix_index[k][l];
				new_entry = true;
				element = hmatrix[k][j]*hmatrix[k][l]; // generalize this to full covariance matrix later
				if (src_index1==src_index2) Rmatrix_diag_temp[src_index1] += element;
				else {
					m=0;
					while ((m < Rmatrix_row_nn[src_index1]) and (new_entry==true)) {
						if (Rmatrix_index_rows[src_index1][m]==src_index2) {
							new_entry = false;
							col_index = m;
						}
						m++;
					}
					if (new_entry) {
						Rmatrix_rows[src_index1].push_back(element);
						Rmatrix_index_rows[src_index1].push_back(src_index2);
						Rmatrix_row_nn[src_index1]++;
						col_i++;
					}
					else Rmatrix_rows[src_index1][col_index] += element;
				}
			}
			Rmatrix_nn_part += col_i;
		}
	}

	for (k=0; k < 2; k++) {
		delete[] hmatrix[k];
		delete[] hmatrix_index[k];
		delete[] hmatrix_row_index[k];
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating Rmatrix: " << wtime << endl;
	}
#endif

	Rmatrix_nn = Rmatrix_nn_part;
	Rmatrix_nn += source_npixels+1;

	Rmatrix = new double[Rmatrix_nn];
	Rmatrix_index = new int[Rmatrix_nn];

	for (i=0; i < source_npixels; i++)
		Rmatrix[i] = Rmatrix_diag_temp[i];

	Rmatrix_index[0] = source_npixels+1;
	for (i=0; i < source_npixels; i++) {
		Rmatrix_index[i+1] = Rmatrix_index[i] + Rmatrix_row_nn[i];
	}

	for (i=0; i < source_npixels; i++) {
		indx = Rmatrix_index[i];
		for (j=0; j < Rmatrix_row_nn[i]; j++) {
			Rmatrix[indx+j] = Rmatrix_rows[i][j];
			Rmatrix_index[indx+j] = Rmatrix_index_rows[i][j];
		}
	}

	delete[] Rmatrix_row_nn;
	delete[] Rmatrix_diag_temp;
	delete[] Rmatrix_rows;
	delete[] Rmatrix_index_rows;

	for (i=0; i < 2; i++) {
		delete[] jvals[i];
		delete[] lvals[i];
	}
}

void QLens::generate_Rmatrix_from_gmatrices()
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	int i,j,k,l,m,n,indx;

	vector<int> *jvals[4];
	vector<int> *lvals[4];
	for (i=0; i < 4; i++) {
		jvals[i] = new vector<int>[source_npixels];
		lvals[i] = new vector<int>[source_npixels];
	}

	Rmatrix_diag_temp = new double[source_npixels];
	Rmatrix_rows = new vector<double>[source_npixels];
	Rmatrix_index_rows = new vector<int>[source_npixels];
	Rmatrix_row_nn = new int[source_npixels];
	Rmatrix_nn = 0;
	int Rmatrix_nn_part = 0;
	for (j=0; j < source_npixels; j++) {
		Rmatrix_diag_temp[j] = 0;
		Rmatrix_row_nn[j] = 0;
	}

	bool new_entry;
	int src_index1, src_index2, col_index, col_i;
	double tmp, element;

	for (k=0; k < 4; k++) {
		gmatrix_rows[k] = new vector<double>[source_npixels];
		gmatrix_index_rows[k] = new vector<int>[source_npixels];
		gmatrix_row_nn[k] = new int[source_npixels];
		gmatrix_nn[k] = 0;
		for (j=0; j < source_npixels; j++) {
			gmatrix_row_nn[k][j] = 0;
		}
	}
	if (source_fit_mode==Delaunay_Source) delaunay_srcgrid->generate_gmatrices();
	else if (source_fit_mode==Cartesian_Source) source_pixel_grid->generate_gmatrices();
	else die("gmatrix not supported for sources other than Delaunay or Cartesian");

	for (k=0; k < 4; k++) {
		gmatrix[k] = new double[gmatrix_nn[k]];
		gmatrix_index[k] = new int[gmatrix_nn[k]];
		gmatrix_row_index[k] = new int[source_npixels+1];

		gmatrix_row_index[k][0] = 0;
		for (i=0; i < source_npixels; i++)
			gmatrix_row_index[k][i+1] = gmatrix_row_index[k][i] + gmatrix_row_nn[k][i];
		if (gmatrix_row_index[k][source_npixels] != gmatrix_nn[k]) die("the number of elements don't match up for gmatrix %i",k);

		for (i=0; i < source_npixels; i++) {
			indx = gmatrix_row_index[k][i];
			for (j=0; j < gmatrix_row_nn[k][i]; j++) {
				gmatrix[k][indx+j] = gmatrix_rows[k][i][j];
				gmatrix_index[k][indx+j] = gmatrix_index_rows[k][i][j];
			}
		}
		delete[] gmatrix_rows[k];
		delete[] gmatrix_index_rows[k];
		delete[] gmatrix_row_nn[k];

		for (i=0; i < source_npixels; i++) {
			for (j=gmatrix_row_index[k][i]; j < gmatrix_row_index[k][i+1]; j++) {
				for (l=j; l < gmatrix_row_index[k][i+1]; l++) {
					src_index1 = gmatrix_index[k][j];
					src_index2 = gmatrix_index[k][l];
					if (src_index1 > src_index2) {
						tmp=src_index1;
						src_index1=src_index2;
						src_index2=tmp;
						jvals[k][src_index1].push_back(l);
						lvals[k][src_index1].push_back(j);
					} else {
						jvals[k][src_index1].push_back(j);
						lvals[k][src_index1].push_back(l);
					}
				}
			}
		}
	}

	#pragma omp parallel for private(i,j,k,l,m,n,src_index1,src_index2,new_entry,col_index,col_i,element) schedule(static) reduction(+:Rmatrix_nn_part)
	for (src_index1=0; src_index1 < source_npixels; src_index1++) {
		for (k=0; k < 4; k++) {
			col_i=0;
			for (n=0; n < jvals[k][src_index1].size(); n++) {
				j = jvals[k][src_index1][n];
				l = lvals[k][src_index1][n];
				src_index2 = gmatrix_index[k][l];
				new_entry = true;
				element = gmatrix[k][j]*gmatrix[k][l]; // generalize this to full covariance matrix later
				if (src_index1==src_index2) Rmatrix_diag_temp[src_index1] += element;
				else {
					m=0;
					while ((m < Rmatrix_row_nn[src_index1]) and (new_entry==true)) {
						if (Rmatrix_index_rows[src_index1][m]==src_index2) {
							new_entry = false;
							col_index = m;
						}
						m++;
					}
					if (new_entry) {
						Rmatrix_rows[src_index1].push_back(element);
						Rmatrix_index_rows[src_index1].push_back(src_index2);
						Rmatrix_row_nn[src_index1]++;
						col_i++;
					}
					else Rmatrix_rows[src_index1][col_index] += element;
				}
			}
			Rmatrix_nn_part += col_i;
		}
	}

	for (k=0; k < 4; k++) {
		delete[] gmatrix[k];
		delete[] gmatrix_index[k];
		delete[] gmatrix_row_index[k];
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating Rmatrix: " << wtime << endl;
	}
#endif

	Rmatrix_nn = Rmatrix_nn_part;
	Rmatrix_nn += source_npixels+1;

	Rmatrix = new double[Rmatrix_nn];
	Rmatrix_index = new int[Rmatrix_nn];

	for (i=0; i < source_npixels; i++)
		Rmatrix[i] = Rmatrix_diag_temp[i];

	Rmatrix_index[0] = source_npixels+1;
	for (i=0; i < source_npixels; i++) {
		Rmatrix_index[i+1] = Rmatrix_index[i] + Rmatrix_row_nn[i];
	}

	for (i=0; i < source_npixels; i++) {
		indx = Rmatrix_index[i];
		for (j=0; j < Rmatrix_row_nn[i]; j++) {
			Rmatrix[indx+j] = Rmatrix_rows[i][j];
			Rmatrix_index[indx+j] = Rmatrix_index_rows[i][j];
		}
	}

	delete[] Rmatrix_row_nn;
	delete[] Rmatrix_diag_temp;
	delete[] Rmatrix_rows;
	delete[] Rmatrix_index_rows;

	for (i=0; i < 4; i++) {
		delete[] jvals[i];
		delete[] lvals[i];
	}
}

void QLens::generate_Rmatrix_from_covariance_kernel(const bool exponential_kernel)
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int ntot = source_npixels*(source_npixels+1)/2;
	Rmatrix_packed.input(ntot);
	if (source_fit_mode==Delaunay_Source) delaunay_srcgrid->generate_covariance_matrix(Rmatrix_packed.array(),kernel_correlation_length,exponential_kernel);
	else die("covariance kernel regularization requires source mode to be 'delaunay'");
	// Right now Rmatrix_packed is in fact the covariance matrix, from which we will do a Cholesky decomposition and then invert to get the Rmatrix
#ifdef USE_MKL
   LAPACKE_dpptrf(LAPACK_ROW_MAJOR,'U',source_npixels,Rmatrix_packed.array()); // Cholesky decomposition
	LAPACKE_dpptri(LAPACK_ROW_MAJOR,'U',source_npixels,Rmatrix_packed.array()); // computes inverse
#else
	repack_Fmatrix_lower();
	bool status = Cholesky_dcmp_packed(Fmatrix_packed.array(),Fmatrix_log_determinant,source_n_amps);
	repack_Fmatrix_upper();
	Cholesky_invert_upper_packed(Rmatrix_packed.array(),source_npixels); // invert the triangular matrix to get U_inverse
	upper_triangular_syrk(Rmatrix_packed.array(),source_npixels); // Now take U_inverse * U_inverse_transpose to get C_inverse (the regularization matrix)
#endif

	/*
	dmatrix cov,cov_inv,prod;
	double *Rmatptr = Rmatrix_packed_copy;
	cov.input(source_npixels,source_npixels);
	cov_inv.input(source_npixels,source_npixels);
	prod.input(source_npixels,source_npixels);
	int i,j;
	for (i=0; i < source_npixels; i++) {
		cov[i][i] = *(Rmatptr++);
		for (j=i+1; j < source_npixels; j++) {
			cov[i][j] = *(Rmatptr++);
			cov[j][i] = cov[i][j];
		}
	}
	Rmatptr = Rmatrix_packed.array();
	for (i=0; i < source_npixels; i++) {
		cov_inv[i][i] = *(Rmatptr++);
		for (j=i+1; j < source_npixels; j++) {
			cov_inv[i][j] = *(Rmatptr++);
			cov_inv[j][i] = cov_inv[i][j];
		}
	}
	prod = cov*cov_inv;
	cout << "THIS SHOULD BE IDENTITY:" << endl;
	for (i=0; i < 2; i++) {
		for (j=0; j < source_npixels; j++) {
			cout << prod[i][j] << " ";
		}
		cout << endl;
	}
	cout << endl;
	delete[] Rmatrix_packed_copy;
	*/
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating covariance kernel Rmatrix: " << wtime << endl;
	}
#endif

}

int SourcePixelGrid::assign_indices_and_count_levels()
{
	levels=1; // we are going to recount the number of levels
	int source_pixel_i=0;
	assign_indices(source_pixel_i);
	return source_pixel_i;
}

void SourcePixelGrid::assign_indices(int& source_pixel_i)
{
	if (levels < level+1) levels=level+1;
	int i, j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->assign_indices(source_pixel_i);
			else {
				cell[i][j]->index = source_pixel_i++;
			}
		}
	}
}

ofstream SourcePixelGrid::index_out;

void SourcePixelGrid::print_indices()
{
	int i, j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->print_indices();
			else {
				index_out << cell[i][j]->index << " " << cell[i][j]->active_index << " level=" << cell[i][j]->level << endl;
			}
		}
	}
}

bool SourcePixelGrid::regrid_if_unmapped_source_subcells;
bool SourcePixelGrid::activate_unmapped_source_pixels;
bool SourcePixelGrid::exclude_source_pixels_outside_fit_window;

int SourcePixelGrid::assign_active_indices_and_count_source_pixels(bool regrid_if_inactive_cells, bool activate_unmapped_pixels, bool exclude_pixels_outside_window)
{
	regrid_if_unmapped_source_subcells = regrid_if_inactive_cells;
	activate_unmapped_source_pixels = activate_unmapped_pixels;
	exclude_source_pixels_outside_fit_window = exclude_pixels_outside_window;
	int source_pixel_i=0;
	assign_active_indices(source_pixel_i);
	return source_pixel_i;
}

void SourcePixelGrid::assign_active_indices(int& source_pixel_i)
{
	int i, j;
	bool unsplit_cell = false;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->assign_active_indices(source_pixel_i);
			else {
					//cell[i][j]->active_index = source_pixel_i++;
					//cell[i][j]->active_pixel = true;
				if (cell[i][j]->maps_to_image_pixel) {
					cell[i][j]->active_index = source_pixel_i++;
					cell[i][j]->active_pixel = true;
				} else {
					if ((lens->mpi_id==0) and (lens->regularization_method == 0)) warn(lens->warnings,"A source pixel does not map to any image pixel (for source pixel %i,%i), level %i, center (%g,%g)",i,j,cell[i][j]->level,cell[i][j]->center_pt[0],cell[i][j]->center_pt[1]); // only show warning if no regularization being used, since matrix cannot be inverted in that case
					if ((activate_unmapped_source_pixels) and ((!regrid_if_unmapped_source_subcells) or (level==0))) { // if we are removing unmapped subpixels, we may still want to activate first-level unmapped pixels
						if ((exclude_source_pixels_outside_fit_window) and (cell[i][j]->maps_to_image_window==false)) ;
						else {
							cell[i][j]->active_index = source_pixel_i++;
							cell[i][j]->active_pixel = true;
						}
					} else {
						cell[i][j]->active_pixel = false;
						if ((regrid_if_unmapped_source_subcells) and (level >= 1)) {
							if (!regrid) regrid = true;
							unsplit_cell = true;
						}
					}
					//if ((exclude_source_pixels_outside_fit_window) and (cell[i][j]->maps_to_image_window==false)) {
						//if (cell[i][j]->active_pixel==true) {
							//source_pixel_i--;
							//if (!regrid) regrid = true;
							//cell[i][j]->active_pixel = false;
						//}
					//}
				}
			}
		}
	}
	if (unsplit_cell) unsplit();
}

SourcePixelGrid::~SourcePixelGrid()
{
	if (cell != NULL) {
		int i,j;
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) delete cell[i][j];
			delete[] cell[i];
		}
		delete[] cell;
		cell = NULL;
	}
}

void SourcePixelGrid::clear()
{
	if (cell == NULL) return;

	int i,j;
	for (i=0; i < u_N; i++) {
		for (j=0; j < w_N; j++) delete cell[i][j];
		delete[] cell[i];
	}
	delete[] cell;
	cell = NULL;
	u_N=1; w_N=1;
}

void SourcePixelGrid::clear_subgrids()
{
	if (level>0) {
		if (cell == NULL) return;
		int i,j;
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				delete cell[i][j];
			}
			delete[] cell[i];
		}
		delete[] cell;
		cell = NULL;
		number_of_pixels -= (u_N*w_N - 1);
		u_N=1; w_N=1;
	} else {
		int i,j;
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				if (cell[i][j]->cell != NULL) cell[i][j]->clear_subgrids();
			}
		}
	}
}

/********************************************* Functions in class DelaunayGrid ***********************************************/

void DelaunayGrid::allocate_multithreaded_variables(const int& threads, const bool reallocate)
{
	if (interpolation_pts[0] != NULL) {
		if (!reallocate) return;
		else deallocate_multithreaded_variables();
	}
	nthreads = threads;
	int i;
	for (i=0; i < 3; i++) interpolation_pts[i] = new lensvector*[nthreads];
}

void DelaunayGrid::deallocate_multithreaded_variables()
{
	if (interpolation_pts[0] != NULL) {
		int i;
		for (i=0; i < 3; i++) delete[] interpolation_pts[i];
		for (i=0; i < 3; i++) interpolation_pts[i] = NULL;
	}
}

DelaunayGrid::DelaunayGrid(QLens* lens_in, double* srcpts_x, double* srcpts_y, const int n_srcpts_in, int *ivals_in, int *jvals_in, const int ni, const int nj) : lens(lens_in)
{
	int threads = 1;
#ifdef USE_OPENMP
	#pragma omp parallel
	{
		#pragma omp master
		threads = omp_get_num_threads();
	}
#endif
	allocate_multithreaded_variables(threads,false); // allocate multithreading arrays ONLY if it hasn't been allocated already (avoids seg faults)
	lens = lens_in;
	if ((lens != NULL) and (lens->image_pixel_grid != NULL)) image_pixel_grid = lens->image_pixel_grid;
	else image_pixel_grid = NULL;

	n_srcpts = n_srcpts_in;
	srcpts = new lensvector[n_srcpts];
	surface_brightness = new double[n_srcpts];
	maps_to_image_pixel = new bool[n_srcpts];
	active_pixel = new bool[n_srcpts];
	active_index = new int[n_srcpts];
	imggrid_ivals = new int[n_srcpts];
	imggrid_jvals = new int[n_srcpts];
	for (int i=0; i < 4; i++) adj_triangles[i] = new int[n_srcpts];
	int n;
	srcpixel_xmin=srcpixel_ymin=1e30;
	srcpixel_xmax=srcpixel_ymax=-1e30;
	for (n=0; n < n_srcpts; n++) {
		srcpts[n][0] = srcpts_x[n];
		srcpts[n][1] = srcpts_y[n];
		//cout << "Sourcept " << n << ": " << srcpts_x[n] << " " << srcpts_y[n] << endl;
		if (srcpts_x[n] > srcpixel_xmax) srcpixel_xmax=srcpts_x[n];
		if (srcpts_y[n] > srcpixel_ymax) srcpixel_ymax=srcpts_y[n];
		if (srcpts_x[n] < srcpixel_xmin) srcpixel_xmin=srcpts_x[n];
		if (srcpts_y[n] < srcpixel_ymin) srcpixel_ymin=srcpts_y[n];
		surface_brightness[n] = 0;
		maps_to_image_pixel[n] = false;
		active_pixel[n] = true;
		active_index[n] = -1;
		imggrid_ivals[n] = ivals_in[n];
		imggrid_jvals[n] = jvals_in[n];
		adj_triangles[0][n] = -1; // +x direction
		adj_triangles[1][n] = -1; // -x direction
		adj_triangles[2][n] = -1; // +y direction
		adj_triangles[3][n] = -1; // -y direction
	}
	img_imin = 30000;
	img_jmin = 30000;
	img_imax = -30000;
	img_jmax = -30000;

	if ((imggrid_ivals != NULL) and (imggrid_jvals != NULL)) {
		// imggrid_ivals and imggrid_jvals aid the ray-tracing by locating a source point, which has an image point near the point
		// being ray-traced; this gives a starting point when searching through the triangles
		int i,j;
		img_ni = ni+1; // add one since we're doing pixel corners
		img_nj = nj+1; // add one since we're doing pixel corners
		img_index_ij = new int*[img_ni];
		for (i=0; i < img_ni; i++) {
			img_index_ij[i] = new int[img_nj];
			for (j=0; j < img_nj; j++) img_index_ij[i][j] = -1; // for any image pixels that don't have a ray-traced point included in the Delaunay grid, the index will be -1
		}

		for (n=0; n < n_srcpts; n++) {
			img_index_ij[imggrid_ivals[n]][imggrid_jvals[n]] = n;
			if (imggrid_ivals[n] < img_imin) img_imin = imggrid_ivals[n];
			if (imggrid_ivals[n] > img_imax) img_imax = imggrid_ivals[n];
			if (imggrid_jvals[n] < img_jmin) img_jmin = imggrid_jvals[n];
			if (imggrid_jvals[n] > img_jmax) img_jmax = imggrid_jvals[n];
		}
	} else {
		img_index_ij = NULL;
	}

	shared_triangles = new vector<int>[n_srcpts];

	Delaunay *delaunay_triangles = new Delaunay(srcpts_x, srcpts_y, n_srcpts);
	delaunay_triangles->Process();
	n_triangles = delaunay_triangles->TriNum();
	triangle = new Triangle[n_triangles];
	delaunay_triangles->store_triangles(triangle);
	avg_area = 0;
	for (n=0; n < n_triangles; n++) {
		shared_triangles[triangle[n].vertex_index[0]].push_back(n);
		shared_triangles[triangle[n].vertex_index[1]].push_back(n);
		shared_triangles[triangle[n].vertex_index[2]].push_back(n);
		avg_area += triangle[n].area;
	}
	avg_area /= n_triangles;
	delete delaunay_triangles;
}

void DelaunayGrid::record_adjacent_triangles_xy()
{
	const double increment = 1e-5;
	int i,j;
	bool foundxp, foundyp, foundxm, foundym;
	double x, y, xp, xm, yp, ym;
	lensvector ptx_p, pty_p, ptx_m, pty_m;
	for (i=0; i < n_srcpts; i++) {
		foundxp = false;
		foundxm = false;
		foundyp = false;
		foundym = false;
		x = srcpts[i][0];
		y = srcpts[i][1];
		xp = x + increment;
		xm = x - increment;
		yp = y + increment;
		ym = y - increment;
		ptx_p.input(xp,y);
		ptx_m.input(xm,y);
		pty_p.input(x,yp);
		pty_m.input(x,ym);

		for (j=0; j < shared_triangles[i].size(); j++) {
			if ((!foundxp) and (test_if_inside(shared_triangles[i][j],ptx_p)==true)) {
				adj_triangles[0][i] = shared_triangles[i][j];
				foundxp = true;
			}
			if ((!foundxm) and (test_if_inside(shared_triangles[i][j],ptx_m)==true)) {
				adj_triangles[1][i] = shared_triangles[i][j];
				foundxm = true;
			}
			if ((!foundyp) and (test_if_inside(shared_triangles[i][j],pty_p)==true)) {
				adj_triangles[2][i] = shared_triangles[i][j];
				foundyp = true;
			}
			if ((!foundym) and (test_if_inside(shared_triangles[i][j],pty_m)==true)) {
				adj_triangles[3][i] = shared_triangles[i][j];
				foundym = true;
			}
		}
		//if (!foundxp) warn("could not find triangle in +x direction for vertex %i",i);
		//if (!foundxm) warn("could not find triangle in -x direction for vertex %i",i);
		//if (!foundyp) warn("could not find triangle in +y direction for vertex %i",i);
		//if (!foundym) warn("could not find triangle in -y direction for vertex %i",i);
	}
}

int DelaunayGrid::search_grid(const int initial_srcpixel, const lensvector& pt, bool& inside_triangle)
{
	if (shared_triangles[initial_srcpixel].size()==0) die("something is really wrong! This vertex doesn't share any triangle sides (vertex %i, ntot=%i)",initial_srcpixel,n_srcpts);
	int n, triangle_num = shared_triangles[initial_srcpixel][0]; // there might be a better way to discern which shared triangle to start with, but we can optimize this later
	if ((pt[0]==srcpts[initial_srcpixel][0]) and (pt[1]==srcpts[initial_srcpixel][1])) {
		inside_triangle = true;
		return triangle_num;
	}

	inside_triangle = false;
	for (n=0; n < n_triangles; n++) {
		if (test_if_inside(triangle_num,pt,inside_triangle)==true) break; // note, will return 'true' if the point is outside the grid but closest to that triangle; 'inside_triangle' flag reveals if it's actually inside the triangle or not
	}
	if (n > n_triangles) die("searched all triangles (or else searched in a loop), still did not find triangle enclosing point--this shouldn't happen! pt=(%g,%g), pixel0=%i",pt[0],pt[1],initial_srcpixel);
	return triangle_num;
}

#define SAME_SIGN(a,b) (((a>0) and (b>0)) or ((a<0) and (b<0)))
bool DelaunayGrid::test_if_inside(int &tri_number, const lensvector& pt, bool& inside_triangle)
{
	// To speed things up, these things can be made static and have an array for each (so each thread uses a different set of static elements)
	lensvector dt1, dt2, dt3;
	double cross_prod;

	Triangle *triptr = &triangle[tri_number];
	int side, same_sign=0;
	bool prod1_samesign = false;
	bool prod2_samesign = false;
	bool prod3_samesign = false;

	dt1[0] = pt[0] - triptr->vertex[0][0];
	dt1[1] = pt[1] - triptr->vertex[0][1];
	dt2[0] = pt[0] - triptr->vertex[1][0];
	dt2[1] = pt[1] - triptr->vertex[1][1];
	dt3[0] = pt[0] - triptr->vertex[2][0];
	dt3[1] = pt[1] - triptr->vertex[2][1];
	cross_prod = dt1[0]*dt2[1] - dt1[1]*dt2[0];
	if SAME_SIGN(cross_prod,triptr->area) { prod1_samesign = true; same_sign++; }
	cross_prod = dt3[0]*dt1[1] - dt3[1]*dt1[0];
	if SAME_SIGN(cross_prod,triptr->area) { prod2_samesign = true; same_sign++; }
	cross_prod = dt2[0]*dt3[1] - dt2[1]*dt3[0];
	if SAME_SIGN(cross_prod,triptr->area) { prod3_samesign = true; same_sign++; }
	if (same_sign==3) {
		inside_triangle = true;
		return true; // point is inside the triangle
	}
	if (same_sign==0) die("none of cross products have same sign as triangle parity (%g vs %g), pt=(%g,%g); this shouldn't happen",cross_prod,triptr->area,pt[0],pt[1]);
	if (same_sign==2) {
		if (!prod1_samesign) side = 2; // on the side opposite vertex 2
		else if (!prod2_samesign) side = 1; // on the side opposite vertex 1
		else side = 0; // on the side opposite vertex 0
	} else { // same_sign = 1
		if (prod1_samesign) side = 0; // near vertex 2, which is between triangles on sides 0 and 1; we pick triangle 0
		else if (prod2_samesign) side = 2; // near vertex 1, which is between triangles on sides 1 and 2; we pick triangle 2
		else side = 1;  // near vertex 0, which is between triangles on sides 0 and 2; we pick triangle 1
	}
	int new_tri_number = triptr->neighbor_index[side];
	if (new_tri_number < 0) {
		if (same_sign==2) {
			inside_triangle = false;
			return true; // there is no triangle on the given side, so this is the closest triangle
		} else {
			if (prod1_samesign) side = 1; //  we tried triangle 0 and it didn't exist, so try triangle 1 next
			else if (prod2_samesign) side = 0; //  we tried triangle 2 and it didn't exist, so try triangle 0 next
			else side = 2;  //  we tried triangle 1 and it didn't exist, so try triangle 2 next
			new_tri_number = triptr->neighbor_index[side];
			if (new_tri_number < 0) {
				inside_triangle = false;
				return true; // we must be at a corner of the grid, so this is the closest triangle
			}
		}
	}
	tri_number = new_tri_number;
	return false; // if returning 'false', we don't bother to set the 'inside_triangle' flag because it will be ignored anyway
}
bool DelaunayGrid::test_if_inside(const int tri_number, const lensvector& pt)
{
	// To speed things up, these things can be made static and have an array for each (so each thread uses a different set of static elements)
	lensvector dt1, dt2, dt3;
	double cross_prod;

	Triangle *triptr = &triangle[tri_number];
	int same_sign=0;
	dt1[0] = pt[0] - triptr->vertex[0][0];
	dt1[1] = pt[1] - triptr->vertex[0][1];
	dt2[0] = pt[0] - triptr->vertex[1][0];
	dt2[1] = pt[1] - triptr->vertex[1][1];
	dt3[0] = pt[0] - triptr->vertex[2][0];
	dt3[1] = pt[1] - triptr->vertex[2][1];
	cross_prod = dt1[0]*dt2[1] - dt1[1]*dt2[0];
	if SAME_SIGN(cross_prod,triptr->area) same_sign++;
	cross_prod = dt3[0]*dt1[1] - dt3[1]*dt1[0];
	if SAME_SIGN(cross_prod,triptr->area) same_sign++;
	cross_prod = dt2[0]*dt3[1] - dt2[1]*dt3[0];
	if SAME_SIGN(cross_prod,triptr->area) same_sign++;
	if (same_sign==3) {
		return true; // point is inside the triangle
	}
	return false; // if returning 'false'
}

#undef SAME_SIGN

int DelaunayGrid::find_closest_vertex(const int tri_number, const lensvector& pt)
{
	// This function effectively allows us to plot the Voronoi cells, since the vertices of the triangles are the "seeds" of the Voronoi cells
	Triangle *triptr = &triangle[tri_number];
	Triangle *neighbor_ptr;
	double distsqr, distsqr_min = 1e30;
	int i,j,k,indx,neighbor_indx;
	for (i=0; i < 3; i++) {
		distsqr = SQR(pt[0] - triptr->vertex[i][0]) + SQR(pt[1] - triptr->vertex[i][1]);
		if (distsqr < distsqr_min) {
			distsqr_min = distsqr;
			indx = triptr->vertex_index[i];
		}
	}
	// Now check neighboring triangles, since it's possible for a point to be closer to a neighboring triangle's vertex,
	// particularly if that point happens to lie close to the edge of the triangle.
	for (i=0; i < 3; i++) {
		if (triptr->neighbor_index[i] != -1) {
			neighbor_ptr = &triangle[triptr->neighbor_index[i]];
			for (j=0; j < 3; j++) {
				neighbor_indx = neighbor_ptr->vertex_index[j];
				if ((neighbor_indx != triptr->vertex_index[0]) and (neighbor_indx != triptr->vertex_index[1]) and (neighbor_indx != triptr->vertex_index[2])) {
					distsqr = SQR(pt[0] - neighbor_ptr->vertex[j][0]) + SQR(pt[1] - neighbor_ptr->vertex[j][1]);
					if (distsqr < distsqr_min) {
						distsqr_min = distsqr;
						indx = neighbor_indx;
					}
				}
			}
		}
	}
	return indx;
}

double DelaunayGrid::sum_edge_sqrlengths(const double min_sb_frac)
{
	lensvector edge;
	double sum=0;
	bool use_sb = false;
	double** sb;
	double min_sb;
	int iv[3];
	int jv[3];
	if ((lens != NULL) and (lens->image_pixel_data != NULL)) {
		use_sb = true;
		sb = lens->image_pixel_data->surface_brightness;
		min_sb = min_sb_frac*lens->image_pixel_data->find_max_sb();
	}
	int i,j;
	// Note, the inside edges (the majority) will be counted twice, but that's ok
	for (i=0; i < n_triangles; i++) {
		if (use_sb) {
			for (j=0; j < 3; j++) {
				iv[j] = imggrid_ivals[triangle[i].vertex_index[j]];
				jv[j] = imggrid_jvals[triangle[i].vertex_index[j]];
			}
			if ((sb[iv[0]][jv[0]] < min_sb) or (sb[iv[1]][jv[1]] < min_sb) or (sb[iv[2]][jv[2]] < min_sb)) continue;
		}
		edge = triangle[i].vertex[1] - triangle[i].vertex[0];
		sum += edge.sqrnorm();
		edge = triangle[i].vertex[2] - triangle[i].vertex[1];
		sum += edge.sqrnorm();
		edge = triangle[i].vertex[0] - triangle[i].vertex[2];
		sum += edge.sqrnorm();
	}
	return sum;
}

void DelaunayGrid::assign_surface_brightness_from_analytic_source()
{
	//cout << "Sourcepts: " << n_srcpts << endl;
	int i,k;
	for (i=0; i < n_srcpts; i++) {
		//cout << "Assigning SB point " << i << "..." << endl;
		surface_brightness[i] = 0;
		for (k=0; k < lens->n_sb; k++) {
			//cout << "source " << k << endl;
			if (lens->sb_list[k]->is_lensed) surface_brightness[i] += lens->sb_list[k]->surface_brightness(srcpts[i][0],srcpts[i][1]);
		}
	}
	for (i=0; i < n_triangles; i++) {
		triangle[i].sb[0] = &surface_brightness[triangle[i].vertex_index[0]];
		triangle[i].sb[1] = &surface_brightness[triangle[i].vertex_index[1]];
		triangle[i].sb[2] = &surface_brightness[triangle[i].vertex_index[2]];
	}
}

void DelaunayGrid::fill_surface_brightness_vector()
{
	int i,j;
	for (i=0, j=0; i < n_srcpts; i++) {
		if (active_pixel[i]) {
			lens->source_pixel_vector[j++] = surface_brightness[i];
		}
	}
}

void DelaunayGrid::update_surface_brightness(int& index)
{
	int i;
	for (i=0; i < n_srcpts; i++) {
		if (active_pixel[i]) {
			surface_brightness[i] = lens->source_pixel_vector[index++];
		} else {
			surface_brightness[i] = 0;
		}
	}
	for (i=0; i < n_triangles; i++) {
		triangle[i].sb[0] = &surface_brightness[triangle[i].vertex_index[0]];
		triangle[i].sb[1] = &surface_brightness[triangle[i].vertex_index[1]];
		triangle[i].sb[2] = &surface_brightness[triangle[i].vertex_index[2]];
	}
}

bool DelaunayGrid::assign_source_mapping_flags(lensvector &input_pt, vector<int>& mapped_delaunay_srcpixels_ij, const int img_pixel_i, const int img_pixel_j, const int thread)
{
	int i,j,k,maxk,n;
	i = img_pixel_i;
	j = img_pixel_j;
	if (i < img_imin) i = img_imin;
	else if (i > img_imax) i = img_imax;
	if (j < img_jmin) j = img_jmin;
	else if (j > img_jmax) j = img_jmax;
	maxk = imax(img_imax-img_imin,img_jmax-img_jmin);
	n=img_index_ij[i][j];
	k=0;
	while (n==-1) {
		// looking for a (ray-traced) pixel within the mask, since the closest pixel doesn't seem to be
		k++;
		if ((j-k >= img_jmin) and (n=img_index_ij[i][j-k]) >= 0) break;
		if ((j+k <= img_jmax) and (n=img_index_ij[i][j+k]) >= 0) break;
		if (i-k >= img_imin) {
			if ((n=img_index_ij[i-k][j]) >= 0) break;
			if ((j-k >= img_jmin) and (n=img_index_ij[i-k][j-k]) >= 0) break;
			if ((j+k <= img_jmax) and (n=img_index_ij[i-k][j+k]) >= 0) break;
		}
		if (i+k <= img_imax) {
			if ((n=img_index_ij[i+k][j]) >= 0) break;
			if ((j-k >= img_jmin) and (n=img_index_ij[i+k][j-k]) >= 0) break;
			if ((j+k <= img_jmax) and (n=img_index_ij[i+k][j+k]) >= 0) break;
		}
		if (k > maxk) {
			warn("could not find a good starting vertex for searching Delaunay grid; starting with vertex 0");
			n=0; // in this case, can't find a good vertex to start with, so we just start with the first one
		}
	}
	//cout << "WTF1" << endl;
	//cout << "searching for point (" << input_pt[0] << "," << input_pt[1] << "), starting with pixel " << n << " (" << srcpts[n][0] << " " << srcpts[n][1] << ")" << endl;
	bool inside_triangle;
	int trinum;
	trinum = search_grid(n,input_pt,inside_triangle);
	//cout << "...found in triangle " << trinum << endl;
	Triangle *triptr = &triangle[trinum];
	//cout << "WTF2" << endl;
	if (!inside_triangle) {
		// we don't want to extrapolate, because it can lead to crazy results outside the grid. so we find the closest vertex and use that vertex's SB
		double sqrdist, sqrdistmin=1e30;
		int kmin;
		for (k=0; k < 3; k++) {
			sqrdist = SQR(input_pt[0]-triptr->vertex[k][0]) + SQR(input_pt[1]-triptr->vertex[k][1]);
			if (sqrdist < sqrdistmin) { sqrdistmin = sqrdist; kmin = k; }
		}
		mapped_delaunay_srcpixels_ij.push_back(triptr->vertex_index[kmin]);
		mapped_delaunay_srcpixels_ij.push_back(triptr->vertex_index[kmin]);
		mapped_delaunay_srcpixels_ij.push_back(triptr->vertex_index[kmin]);
		maps_to_image_pixel[triptr->vertex_index[kmin]] = true;
		//outfile << img_pixel_i << " " << img_pixel_j << " " << triptr->vertex_index[kmin] << endl;
	} else {
		mapped_delaunay_srcpixels_ij.push_back(triptr->vertex_index[0]);
		mapped_delaunay_srcpixels_ij.push_back(triptr->vertex_index[1]);
		mapped_delaunay_srcpixels_ij.push_back(triptr->vertex_index[2]);
		//outfile << img_pixel_i << " " << img_pixel_j << " " << triptr->vertex_index[0] << endl;
		//outfile << img_pixel_i << " " << img_pixel_j << " " << triptr->vertex_index[1] << endl;
		//outfile << img_pixel_i << " " << img_pixel_j << " " << triptr->vertex_index[2] << endl;
		//cout << "WTF3" << endl;
		maps_to_image_pixel[triptr->vertex_index[0]] = true;
		maps_to_image_pixel[triptr->vertex_index[1]] = true;
		maps_to_image_pixel[triptr->vertex_index[2]] = true;
	}
	return true;
}

void DelaunayGrid::calculate_Lmatrix(const int img_index, const int image_pixel_i, const int image_pixel_j, int& index, lensvector &input_pt, const int& ii, const double weight, const int& thread)
{
	int vertex_index;
	for (int i=0; i < 3; i++) {
		vertex_index = image_pixel_grid->mapped_delaunay_srcpixels[image_pixel_i][image_pixel_j][3*ii+i];
		lens->Lmatrix_index_rows[img_index].push_back(vertex_index);
		interpolation_pts[i][thread] = &srcpts[vertex_index];
	}

	double d = ((*interpolation_pts[0][thread])[0]-(*interpolation_pts[1][thread])[0])*((*interpolation_pts[1][thread])[1]-(*interpolation_pts[2][thread])[1]) - ((*interpolation_pts[1][thread])[0]-(*interpolation_pts[2][thread])[0])*((*interpolation_pts[0][thread])[1]-(*interpolation_pts[1][thread])[1]);
	if (d==0) {
		// in this case the points are all the same
		lens->Lmatrix_rows[img_index].push_back(1);
		lens->Lmatrix_rows[img_index].push_back(0);
		lens->Lmatrix_rows[img_index].push_back(0);
	} else {
		lens->Lmatrix_rows[img_index].push_back(weight*(input_pt[0]*((*interpolation_pts[1][thread])[1]-(*interpolation_pts[2][thread])[1]) + input_pt[1]*((*interpolation_pts[2][thread])[0]-(*interpolation_pts[1][thread])[0]) + (*interpolation_pts[1][thread])[0]*(*interpolation_pts[2][thread])[1] - (*interpolation_pts[1][thread])[1]*(*interpolation_pts[2][thread])[0])/d);
		lens->Lmatrix_rows[img_index].push_back(weight*(input_pt[0]*((*interpolation_pts[2][thread])[1]-(*interpolation_pts[0][thread])[1]) + input_pt[1]*((*interpolation_pts[0][thread])[0]-(*interpolation_pts[2][thread])[0]) + (*interpolation_pts[0][thread])[1]*(*interpolation_pts[2][thread])[0] - (*interpolation_pts[0][thread])[0]*(*interpolation_pts[2][thread])[1])/d);
		lens->Lmatrix_rows[img_index].push_back(weight*(input_pt[0]*((*interpolation_pts[0][thread])[1]-(*interpolation_pts[1][thread])[1]) + input_pt[1]*((*interpolation_pts[1][thread])[0]-(*interpolation_pts[0][thread])[0]) + (*interpolation_pts[0][thread])[0]*(*interpolation_pts[1][thread])[1] - (*interpolation_pts[0][thread])[1]*(*interpolation_pts[1][thread])[0])/d);
	}

	index += 3;
}

double DelaunayGrid::find_lensed_surface_brightness(lensvector &input_pt, const int img_pixel_i, const int img_pixel_j, const int thread)
{
	lensvector *pts[3];
	double *sb[3];

	int i,j,k,maxk,n;
	i = img_pixel_i;
	j = img_pixel_j;
	if (i < img_imin) i = img_imin;
	else if (i > img_imax) i = img_imax;
	if (j < img_jmin) j = img_jmin;
	else if (j > img_jmax) j = img_jmax;
	maxk = imax(img_imax-img_imin,img_jmax-img_jmin);
	n=img_index_ij[i][j];
	k=0;
	while (n==-1) {
		// looking for a (ray-traced) pixel within the mask, since the closest pixel doesn't seem to be
		k++;
		if ((j-k >= img_jmin) and (n=img_index_ij[i][j-k]) >= 0) break;
		if ((j+k <= img_jmax) and (n=img_index_ij[i][j+k]) >= 0) break;
		if (i-k >= img_imin) {
			if ((n=img_index_ij[i-k][j]) >= 0) break;
			if ((j-k >= img_jmin) and (n=img_index_ij[i-k][j-k]) >= 0) break;
			if ((j+k <= img_jmax) and (n=img_index_ij[i-k][j+k]) >= 0) break;
		}
		if (i+k <= img_imax) {
			if ((n=img_index_ij[i+k][j]) >= 0) break;
			if ((j-k >= img_jmin) and (n=img_index_ij[i+k][j-k]) >= 0) break;
			if ((j+k <= img_jmax) and (n=img_index_ij[i+k][j+k]) >= 0) break;
		}
		if (k > maxk) {
			warn("could not find a good starting vertex for searching Delaunay grid; starting with vertex 0");
			n=0; // in this case, can't find a good vertex to start with, so we just start with the first one
		}
	}

	bool inside_triangle;
	int trinum = search_grid(n,input_pt,inside_triangle);
	Triangle *triptr = &triangle[trinum];
	if (!inside_triangle) {
		// we don't want to extrapolate, because it can lead to crazy results outside the grid. so we find the closest vertex and use that vertex's SB
		double sqrdist, sqrdistmin=1e30;
		int kmin;
		for (k=0; k < 3; k++) {
			sqrdist = SQR(input_pt[0]-triptr->vertex[k][0]) + SQR(input_pt[1]-triptr->vertex[k][1]);
			if (sqrdist < sqrdistmin) { sqrdistmin = sqrdist; kmin = k; }
		}
		return *triptr->sb[kmin];
	}
	sb[0] = triptr->sb[0];
	sb[1] = triptr->sb[1];
	sb[2] = triptr->sb[2];
	pts[0] = &triptr->vertex[0];
	pts[1] = &triptr->vertex[1];
	pts[2] = &triptr->vertex[2];

	double d, total_sb = 0;
	d = ((*pts[0])[0]-(*pts[1])[0])*((*pts[1])[1]-(*pts[2])[1]) - ((*pts[1])[0]-(*pts[2])[0])*((*pts[0])[1]-(*pts[1])[1]);
	total_sb += (*sb[0])*(input_pt[0]*((*pts[1])[1]-(*pts[2])[1]) + input_pt[1]*((*pts[2])[0]-(*pts[1])[0]) + (*pts[1])[0]*(*pts[2])[1] - (*pts[1])[1]*(*pts[2])[0]);
	total_sb += (*sb[1])*(input_pt[0]*((*pts[2])[1]-(*pts[0])[1]) + input_pt[1]*((*pts[0])[0]-(*pts[2])[0]) + (*pts[0])[1]*(*pts[2])[0] - (*pts[0])[0]*(*pts[2])[1]);
	total_sb += (*sb[2])*(input_pt[0]*((*pts[0])[1]-(*pts[1])[1]) + input_pt[1]*((*pts[1])[0]-(*pts[0])[0]) + (*pts[0])[0]*(*pts[1])[1] - (*pts[0])[1]*(*pts[1])[0]);
	total_sb /= d;
	return total_sb;
}

double DelaunayGrid::interpolate_surface_brightness(lensvector &input_pt)
{
	lensvector *pts[3];
	double *sb[3];
	bool inside_triangle;
	int trinum = search_grid(0,input_pt,inside_triangle);
	Triangle *triptr = &triangle[trinum];
	if (!inside_triangle) {
		// we don't want to extrapolate, because it can lead to crazy results outside the grid. so we find the closest vertex and use that vertex's SB
		double sqrdist, sqrdistmin=1e30;
		int kmin;
		for (int k=0; k < 3; k++) {
			sqrdist = SQR(input_pt[0]-triptr->vertex[k][0]) + SQR(input_pt[1]-triptr->vertex[k][1]);
			if (sqrdist < sqrdistmin) { sqrdistmin = sqrdist; kmin = k; }
		}
		return *triptr->sb[kmin];
	}
	sb[0] = triptr->sb[0];
	sb[1] = triptr->sb[1];
	sb[2] = triptr->sb[2];
	pts[0] = &triptr->vertex[0];
	pts[1] = &triptr->vertex[1];
	pts[2] = &triptr->vertex[2];

	double d, total_sb = 0;
	d = ((*pts[0])[0]-(*pts[1])[0])*((*pts[1])[1]-(*pts[2])[1]) - ((*pts[1])[0]-(*pts[2])[0])*((*pts[0])[1]-(*pts[1])[1]);
	total_sb += (*sb[0])*(input_pt[0]*((*pts[1])[1]-(*pts[2])[1]) + input_pt[1]*((*pts[2])[0]-(*pts[1])[0]) + (*pts[1])[0]*(*pts[2])[1] - (*pts[1])[1]*(*pts[2])[0]);
	total_sb += (*sb[1])*(input_pt[0]*((*pts[2])[1]-(*pts[0])[1]) + input_pt[1]*((*pts[0])[0]-(*pts[2])[0]) + (*pts[0])[1]*(*pts[2])[0] - (*pts[0])[0]*(*pts[2])[1]);
	total_sb += (*sb[2])*(input_pt[0]*((*pts[0])[1]-(*pts[1])[1]) + input_pt[1]*((*pts[1])[0]-(*pts[0])[0]) + (*pts[0])[0]*(*pts[1])[1] - (*pts[0])[1]*(*pts[1])[0]);
	total_sb /= d;
	return total_sb;
}

int DelaunayGrid::assign_active_indices_and_count_source_pixels(const bool activate_unmapped_pixels)
{
	int source_pixel_i=0;
	for (int i=0; i < n_srcpts; i++) {
		active_pixel[i] = false;
		if ((maps_to_image_pixel[i]) or (activate_unmapped_pixels)) {
			active_pixel[i] = true;
			active_index[i] = source_pixel_i++;
		} else {
			if ((lens->mpi_id==0) and (lens->regularization_method == 0)) warn(lens->warnings,"A source pixel does not map to any image pixel (for source pixel %i), center (%g,%g)",i,srcpts[i][0],srcpts[i][1]); // only show warning if no regularization being used, since matrix cannot be inverted in that case
		}

	}
	return source_pixel_i;
}

void DelaunayGrid::generate_hmatrices()
{
	// NOTE: for the moment, we are assuming all the source pixels are 'active', i.e. will be used in the inversion
	record_adjacent_triangles_xy();

	auto add_hmatrix_entry = [](QLens *lens, const int l, const int i, const int j, const double entry)
	{
		int dup = false;
		for (int k=0; k < lens->hmatrix_row_nn[l][i]; k++) {
			if (lens->hmatrix_index_rows[l][i][k]==j) {
				lens->hmatrix_rows[l][i][k] += entry;
				dup = true;
				break;
			}
		}
		if (!dup) {
			lens->hmatrix_rows[l][i].push_back(entry);
			lens->hmatrix_index_rows[l][i].push_back(j);
			lens->hmatrix_row_nn[l][i]++;
			lens->hmatrix_nn[l]++;
		}
	};


	int i,j,k,l;
	int vertex_i1, vertex_i2, trinum;
	Triangle* triptr;
	bool found_i1, found_i2;
	double x1, y1, x2, y2, dpt, dpt1, dpt2, dpt12;
	double length, minlength, avg_length;
	avg_length = sqrt(avg_area);
	lensvector pt;
	for (i=0; i < n_srcpts; i++) {
		for (j=0; j < 4; j++) {
			if (j > 1) l = 1;
			else l = 0;
			vertex_i1 = -1;
			vertex_i2 = -1;
			if ((trinum = adj_triangles[j][i]) != -1) {
				triptr = &triangle[trinum];
				found_i1 = false;
				found_i2 = false;
				if ((k = triptr->vertex_index[0]) != i) { vertex_i1 = k; found_i1 = true; }
				if ((k = triptr->vertex_index[1]) != i) {
					if (!found_i1) {
						vertex_i1 = k;
						found_i1 = true;
					} else {
						vertex_i2 = k;
						found_i2 = true;
					}
				}
				if (!found_i1) die("WHAT?! couldn't find more than one vertex that isn't the one in question");
				if ((!found_i2) and ((k = triptr->vertex_index[2]) != i)) {
					vertex_i2 = k;
					found_i2 = true;
				}
				if (!found_i2) die("WHAT?! couldn't find both vertices that aren't the one in question");
			}
			if (vertex_i1 != -1) {
				x1 = srcpts[vertex_i1][0];
				y1 = srcpts[vertex_i1][1];
				x2 = srcpts[vertex_i2][0];
				y2 = srcpts[vertex_i2][1];
				if (j < 2) {
					pt[1] = srcpts[i][1];
					pt[0] = ((x2-x1)/(y2-y1))*(pt[1]-y1) + x1;
					dpt = abs(pt[0]-srcpts[i][0]);
				} else {
					pt[0] = srcpts[i][0];
					pt[1] = ((y2-y1)/(x2-x1))*(pt[0]-x1) + y1;
					dpt = abs(pt[1]-srcpts[i][1]);
				}
				dpt12 = sqrt(SQR(x2-x1) + SQR(y2-y1));
				dpt1 = sqrt(SQR(pt[0]-x1)+SQR(pt[1]-y1));
				dpt2 = sqrt(SQR(pt[0]-x2)+SQR(pt[1]-y2));
				// we scale hmatrix by the average triangle length so the regularization parameter is dimensionless
				add_hmatrix_entry(lens,l,i,i,-avg_length/dpt);
				add_hmatrix_entry(lens,l,i,vertex_i1,avg_length*dpt2/(dpt*dpt12));
				add_hmatrix_entry(lens,l,i,vertex_i2,avg_length*dpt1/(dpt*dpt12));
			} else {
				minlength=1e30;
				for (k=0; k < shared_triangles[i].size(); k++) {
					triptr = &triangle[shared_triangles[i][k]];
					length = sqrt(triptr->area);
					if (length < minlength) minlength = length;
				}
				add_hmatrix_entry(lens,l,i,i,-avg_length/minlength);
				//add_hmatrix_entry(lens,l,i,i,sqrt(1/2.0)/2);
			}
		}
	}
}

void DelaunayGrid::generate_gmatrices()
{
	// NOTE: for the moment, we are assuming all the source pixels are 'active', i.e. will be used in the inversion
	record_adjacent_triangles_xy();

	auto add_gmatrix_entry = [](QLens *lens, const int l, const int i, const int j, const double entry)
	{
		int dup = false;
		for (int k=0; k < lens->gmatrix_row_nn[l][i]; k++) {
			if (lens->gmatrix_index_rows[l][i][k]==j) {
				lens->gmatrix_rows[l][i][k] += entry;
				dup = true;
				break;
			}
		}
		if (!dup) {
			lens->gmatrix_rows[l][i].push_back(entry);
			lens->gmatrix_index_rows[l][i].push_back(j);
			lens->gmatrix_row_nn[l][i]++;
			lens->gmatrix_nn[l]++;
		}
	};


	int i,k,l;
	int vertex_i1, vertex_i2, trinum;
	Triangle* triptr;
	bool found_i1, found_i2;
	double length, minlength;
	double x1, y1, x2, y2, dpt, dpt1, dpt2, dpt12;
	lensvector pt;
	for (i=0; i < n_srcpts; i++) {
		for (l=0; l < 4; l++) {
			vertex_i1 = -1;
			vertex_i2 = -1;
			if ((trinum = adj_triangles[l][i]) != -1) {
				triptr = &triangle[trinum];
				found_i1 = false;
				found_i2 = false;
				if ((k = triptr->vertex_index[0]) != i) { vertex_i1 = k; found_i1 = true; }
				if ((k = triptr->vertex_index[1]) != i) {
					if (!found_i1) {
						vertex_i1 = k;
						found_i1 = true;
					} else {
						vertex_i2 = k;
						found_i2 = true;
					}
				}
				if (!found_i1) die("WHAT?! couldn't find more than one vertex that isn't the one in question");
				if ((!found_i2) and ((k = triptr->vertex_index[2]) != i)) {
					vertex_i2 = k;
					found_i2 = true;
				}
				if (!found_i2) die("WHAT?! couldn't find both vertices that aren't the one in question");
			}
			if (vertex_i1 != -1) {
				x1 = srcpts[vertex_i1][0];
				y1 = srcpts[vertex_i1][1];
				x2 = srcpts[vertex_i2][0];
				y2 = srcpts[vertex_i2][1];
				if (l < 2) {
					pt[1] = srcpts[i][1];
					pt[0] = ((x2-x1)/(y2-y1))*(pt[1]-y1) + x1;
					dpt = abs(pt[0]-srcpts[i][0]);
				} else {
					pt[0] = srcpts[i][0];
					pt[1] = ((y2-y1)/(x2-x1))*(pt[0]-x1) + y1;
					dpt = abs(pt[1]-srcpts[i][1]);
				}
				dpt12 = sqrt(SQR(x2-x1) + SQR(y2-y1));
				dpt1 = sqrt(SQR(pt[0]-x1)+SQR(pt[1]-y1));
				dpt2 = sqrt(SQR(pt[0]-x2)+SQR(pt[1]-y2));
				add_gmatrix_entry(lens,l,i,i,1.0);
				add_gmatrix_entry(lens,l,i,vertex_i1,-dpt2/(dpt12));
				add_gmatrix_entry(lens,l,i,vertex_i2,-dpt1/(dpt12));
				//add_gmatrix_entry(lens,l,i,i,sqrt(1/2.0)/2);
			} else {
				minlength=1e30;
				for (k=0; k < shared_triangles[i].size(); k++) {
					triptr = &triangle[shared_triangles[i][k]];
					length = sqrt(triptr->area);
					if (length < minlength) minlength = length;
				}
				add_gmatrix_entry(lens,l,i,i,1.0);
				//add_gmatrix_entry(lens,l,i,i,sqrt(1/2.0)/2);
			}
		}
	}
}

void DelaunayGrid::generate_covariance_matrix(double *cov_matrix_packed, const double corr_length, const bool exponential_kernel)
{
	int i,j;
	double sqrdist;
	const double epsilon = 1e-7;
	for (i=0; i < n_srcpts; i++) {
		if (exponential_kernel) *(cov_matrix_packed++) = 1.0;
		else *(cov_matrix_packed++) = 1.0 + epsilon; // adding epsilon to diagonal reduces numerical error during inversion by increasing the smallest eigenvalues
		for (j=i+1; j < n_srcpts; j++) {
			sqrdist = SQR(srcpts[i][0]-srcpts[j][0]) + SQR(srcpts[i][1]-srcpts[j][1]);
			if (exponential_kernel) *(cov_matrix_packed++) = exp(-sqrt(sqrdist)/corr_length); // exponential kernel
			else *(cov_matrix_packed++) = exp(-sqrdist/(2*corr_length*corr_length));
		}
	}
}

void DelaunayGrid::plot_surface_brightness(string root, const double xmin, const double xmax, const double ymin, const double ymax, const double grid_scalefac, const int npix, const bool interpolate_sb)
{
	string img_filename = root + ".dat";
	string x_filename = root + ".x";
	string y_filename = root + ".y";

	double x, y, xlength, ylength, pixel_xlength, pixel_ylength;
	int i, j, npts_x, npts_y;
	xlength = xmax-xmin;
	ylength = ymax-ymin;
	npts_x = (int) npix*sqrt(xlength/ylength);
	npts_y = (int) npts_x*ylength/xlength;
	pixel_xlength = xlength/npts_x;
	pixel_ylength = ylength/npts_y;

	ofstream pixel_xvals; lens->open_output_file(pixel_xvals,x_filename);
	for (i=0, x=xmin; i <= npts_x; i++, x += pixel_xlength) pixel_xvals << x << endl;

	ofstream pixel_yvals; lens->open_output_file(pixel_yvals,y_filename);
	for (i=0, y=ymin; i <= npts_y; i++, y += pixel_ylength) pixel_yvals << y << endl;

	ofstream pixel_surface_brightness_file;
	lens->open_output_file(pixel_surface_brightness_file,img_filename.c_str());
	int srcpt_i, trinum;
	double sb;
	lensvector pt;
	for (j=0, y=ymin+pixel_xlength/2; j < npts_y; j++, y += pixel_ylength) {
		pt[1] = y;
		for (i=0, x=xmin+pixel_xlength/2; i < npts_x; i++, x += pixel_xlength) {
			pt[0] = x;
			if (interpolate_sb) {
				sb = interpolate_surface_brightness(pt);
			} else {
				// The following lines will plot the Voronoi cells that are dual to the Delaunay triangulation. Note however, that when SB interpolation is
				// performed during ray-tracing, we use the vertices of the triangle that a point lands in, which may not include the closest vertex (i.e. the
				// Voronoi cell it lies in). Thus, the Voronoi cells are for visualization only, and do not directly show what the ray-traced SB will look like.
				bool inside_triangle;
				trinum = search_grid(0,pt,inside_triangle); // maybe you can speed this up later by choosing a better initial triangle
				srcpt_i = find_closest_vertex(trinum,pt);
				sb = surface_brightness[srcpt_i];
			}
			//cout << x << " " << y << " " << srcpts[srcpt_i][0] << " " << srcpts[srcpt_i][1] << endl;
			pixel_surface_brightness_file << sb << " ";
		}
		pixel_surface_brightness_file << endl;
	}
	string srcpt_filename = root + "_srcpts.dat";
	ofstream srcout; lens->open_output_file(srcout,srcpt_filename);
	for (i=0; i < n_srcpts; i++) {
		srcout << srcpts[i][0] << " " << srcpts[i][1] << endl;
	}
}

DelaunayGrid::~DelaunayGrid()
{
	delete[] srcpts;
	delete[] surface_brightness;
	delete[] triangle;
	delete[] maps_to_image_pixel;
	delete[] active_pixel;
	delete[] active_index;
	delete[] shared_triangles;
	delete[] imggrid_ivals;
	delete[] imggrid_jvals;
	delete[] adj_triangles[0];
	delete[] adj_triangles[1];
	delete[] adj_triangles[2];
	delete[] adj_triangles[3];
	if (img_index_ij != NULL) {
		for (int i=0; i < img_ni; i++) delete[] img_index_ij[i];
		delete[] img_index_ij;
	}
}

/******************************** Functions in class ImagePixelData, and FITS file functions *********************************/

void ImagePixelData::load_data(string root)
{
	string sbfilename = root + ".dat";
	string xfilename = root + ".x";
	string yfilename = root + ".y";

	int i,j;
	double dummy;
	if (xvals != NULL) delete[] xvals;
	if (yvals != NULL) delete[] yvals;
	if (surface_brightness != NULL) {
		for (i=0; i < npixels_x; i++) delete[] surface_brightness[i];
		delete[] surface_brightness;
	}
	if (high_sn_pixel != NULL) {
		for (i=0; i < npixels_x; i++) delete[] high_sn_pixel[i];
		delete[] high_sn_pixel;
	}
	if (in_mask != NULL) {
		for (i=0; i < npixels_x; i++) delete[] in_mask[i];
		delete[] in_mask;
	}
	if (extended_mask != NULL) {
		for (i=0; i < npixels_x; i++) delete[] extended_mask[i];
		delete[] extended_mask;
	}
	if (foreground_mask != NULL) {
		for (i=0; i < npixels_x; i++) delete[] foreground_mask[i];
		delete[] foreground_mask;
	}

	ifstream xfile(xfilename.c_str());
	i=0;
	while (xfile >> dummy) i++;
	xfile.close();
	npixels_x = i-1;

	ifstream yfile(yfilename.c_str());
	j=0;
	while (yfile >> dummy) j++;
	yfile.close();
	npixels_y = j-1;

	n_required_pixels = npixels_x*npixels_y;
	n_high_sn_pixels = n_required_pixels; // this will be recalculated in assign_high_sn_pixels() function
	xvals = new double[npixels_x+1];
	xfile.open(xfilename.c_str());
	for (i=0; i <= npixels_x; i++) xfile >> xvals[i];
	yvals = new double[npixels_y+1];
	yfile.open(yfilename.c_str());
	for (i=0; i <= npixels_y; i++) yfile >> yvals[i];

	pixel_xcvals = new double[npixels_x];
	pixel_ycvals = new double[npixels_y];
	for (i=0; i < npixels_x; i++) {
		pixel_xcvals[i] = (xvals[i]+xvals[i+1])/2;
	}
	for (i=0; i < npixels_y; i++) {
		pixel_ycvals[i] = (yvals[i]+yvals[i+1])/2;
	}

	ifstream sbfile(sbfilename.c_str());
	surface_brightness = new double*[npixels_x];
	high_sn_pixel = new bool*[npixels_x];
	in_mask = new bool*[npixels_x];
	extended_mask = new bool*[npixels_x];
	foreground_mask = new bool*[npixels_x];
	for (i=0; i < npixels_x; i++) {
		surface_brightness[i] = new double[npixels_y];
		high_sn_pixel[i] = new bool[npixels_y];
		in_mask[i] = new bool[npixels_y];
		extended_mask[i] = new bool[npixels_y];
		foreground_mask[i] = new bool[npixels_y];
		for (j=0; j < npixels_y; j++) {
			in_mask[i][j] = true;
			extended_mask[i][j] = true;
			foreground_mask[i][j] = true;
			high_sn_pixel[i][j] = true;
			sbfile >> surface_brightness[i][j];
		}
	}
	find_extended_mask_rmax(); // used when splining integrals for deflection/hessian from Fourier modes
	assign_high_sn_pixels();
}

void ImagePixelData::load_from_image_grid(ImagePixelGrid* image_pixel_grid, const double noise_in)
{
	int i,j;
	if (xvals != NULL) delete[] xvals;
	if (yvals != NULL) delete[] yvals;
	if (pixel_xcvals != NULL) delete[] pixel_xcvals;
	if (pixel_ycvals != NULL) delete[] pixel_ycvals;
	if (surface_brightness != NULL) {
		for (i=0; i < npixels_x; i++) delete[] surface_brightness[i];
		delete[] surface_brightness;
	}
	if (high_sn_pixel != NULL) {
		for (i=0; i < npixels_x; i++) delete[] high_sn_pixel[i];
		delete[] high_sn_pixel;
	}
	if (in_mask != NULL) {
		for (i=0; i < npixels_x; i++) delete[] in_mask[i];
		delete[] in_mask;
	}
	if (extended_mask != NULL) {
		for (i=0; i < npixels_x; i++) delete[] extended_mask[i];
		delete[] extended_mask;
	}
	if (foreground_mask != NULL) {
		for (i=0; i < npixels_x; i++) delete[] foreground_mask[i];
		delete[] foreground_mask;
	}
	if ((QLens::setup_fft_convolution) and (lens != NULL)) lens->cleanup_FFT_convolution_arrays(); // since number of image pixels has changed, will need to redo FFT setup

	npixels_x = image_pixel_grid->x_N;
	npixels_y = image_pixel_grid->y_N;
	xmin = image_pixel_grid->xmin;
	xmax = image_pixel_grid->xmax;
	ymin = image_pixel_grid->ymin;
	ymax = image_pixel_grid->ymax;

	n_required_pixels = npixels_x*npixels_y;
	n_high_sn_pixels = n_required_pixels; // this will be recalculated in assign_high_sn_pixels() function
	xvals = new double[npixels_x+1];
	for (i=0; i <= npixels_x; i++) xvals[i] = image_pixel_grid->corner_pts[i][0][0];
	yvals = new double[npixels_y+1];
	for (i=0; i <= npixels_y; i++) yvals[i] = image_pixel_grid->corner_pts[0][i][1];

	pixel_xcvals = new double[npixels_x];
	pixel_ycvals = new double[npixels_y];
	for (i=0; i < npixels_x; i++) {
		pixel_xcvals[i] = (xvals[i]+xvals[i+1])/2;
	}
	for (i=0; i < npixels_y; i++) {
		pixel_ycvals[i] = (yvals[i]+yvals[i+1])/2;
	}

	double xstep = pixel_xcvals[1] - pixel_xcvals[0];
	double ystep = pixel_ycvals[1] - pixel_ycvals[0];
	pixel_size = dmin(xstep,ystep);

	surface_brightness = new double*[npixels_x];
	high_sn_pixel = new bool*[npixels_x];
	in_mask = new bool*[npixels_x];
	extended_mask = new bool*[npixels_x];
	foreground_mask = new bool*[npixels_x];
	for (i=0; i < npixels_x; i++) {
		surface_brightness[i] = new double[npixels_y];
		high_sn_pixel[i] = new bool[npixels_y];
		in_mask[i] = new bool[npixels_y];
		extended_mask[i] = new bool[npixels_y];
		foreground_mask[i] = new bool[npixels_y];
		for (j=0; j < npixels_y; j++) {
			in_mask[i][j] = true;
			extended_mask[i][j] = true;
			foreground_mask[i][j] = true;
			high_sn_pixel[i][j] = true;
			surface_brightness[i][j] = image_pixel_grid->surface_brightness[i][j];
		}
	}
	pixel_noise = noise_in;
	find_extended_mask_rmax(); // used when splining integrals for deflection/hessian from Fourier modes
	assign_high_sn_pixels();
}

bool ImagePixelData::load_data_fits(bool use_pixel_size, string fits_filename)
{
#ifndef USE_FITS
	cout << "FITS capability disabled; QLens must be compiled with the CFITSIO library to read FITS files\n"; return false;
#else
	bool image_load_status = false;
	int i,j,kk;
	fitsfile *fptr;   // FITS file pointer, defined in fitsio.h
	int status = 0;   // CFITSIO status value MUST be initialized to zero!
	int bitpix, naxis;
	long naxes[2] = {1,1};
	double *pixels;
	double x, y, xstep, ystep;

	int hdutype;
	if (!fits_open_file(&fptr, fits_filename.c_str(), READONLY, &status))
	{
		 //if ( fits_movabs_hdu(fptr, 2, &hdutype, &status) ) /* move to 2nd HDU */
			//die("fuck");

		if (xvals != NULL) delete[] xvals;
		if (yvals != NULL) delete[] yvals;
		if (surface_brightness != NULL) {
			for (i=0; i < npixels_x; i++) delete[] surface_brightness[i];
			delete[] surface_brightness;
		}
		if (high_sn_pixel != NULL) {
			for (i=0; i < npixels_x; i++) delete[] high_sn_pixel[i];
			delete[] high_sn_pixel;
		}
		int nkeys;
		fits_get_hdrspace(fptr, &nkeys, NULL, &status); // get # of keywords

		int ii;
		char card[FLEN_CARD];   // Standard string lengths defined in fitsio.h

		bool reading_qlens_comment = false;
		bool reading_markers = false;
		bool pixel_size_found = false;
		int pos, pos1;
		for (ii = 1; ii <= nkeys; ii++) { // Read and print each keywords 
			if (fits_read_record(fptr, ii, card, &status))break;
			// When you get time: put pixel size and pixel noise as lines in the FITS file comment! Then have it load them here so you don't need to specify them as separate lines.
			string cardstring(card);
			if (reading_qlens_comment) {
				if ((pos = cardstring.find("COMMENT")) != string::npos) {
					if (((pos1 = cardstring.find("mk: ")) != string::npos) or ((pos1 = cardstring.find("MK: ")) != string::npos)) {
						reading_markers = true;
						reading_qlens_comment = false;
						if (lens != NULL) lens->param_markers = cardstring.substr(pos1+4);
					} else {
						if (lens != NULL) lens->data_info += cardstring.substr(pos+8);
					}
				} else break;
			} else if (reading_markers) {
				if ((pos = cardstring.find("COMMENT")) != string::npos) {
					if (lens != NULL) lens->param_markers += cardstring.substr(pos+8);
					// A potential issue is that if there are enough markers to fill more than one line, there might be an extra space inserted,
					// in which case the markers won't come out properly. No time to deal with this now, but something to look out for.
				} else break;
			} else if (((pos = cardstring.find("ql: ")) != string::npos) or ((pos = cardstring.find("QL: ")) != string::npos)) {
				reading_qlens_comment = true;
				if (lens != NULL) lens->data_info = cardstring.substr(pos+4);
			} else if (((pos = cardstring.find("mk: ")) != string::npos) or ((pos = cardstring.find("MK: ")) != string::npos)) {
				reading_markers = true;
				if (lens != NULL) lens->param_markers = cardstring.substr(pos+4);
			} else if (cardstring.find("PXSIZE ") != string::npos) {
				string pxsize_string = cardstring.substr(11);
				stringstream pxsize_str;
				pxsize_str << pxsize_string;
				pxsize_str >> pixel_size;
				if (lens != NULL) lens->data_pixel_size = pixel_size;
				pixel_size_found = true;
				if (!use_pixel_size) use_pixel_size = true;
			} else if (cardstring.find("PXNOISE ") != string::npos) {
				string pxnoise_string = cardstring.substr(11);
				stringstream pxnoise_str;
				pxnoise_str << pxnoise_string;
				pxnoise_str >> pixel_noise;
				if (lens != NULL) {
					lens->data_pixel_noise = pixel_noise;
					SB_Profile::SB_noise = pixel_noise;
				}
			} else if (cardstring.find("PSFSIG ") != string::npos) {
				string psfwidth_string = cardstring.substr(11);
				stringstream psfwidth_str;
				psfwidth_str << psfwidth_string;
				if (lens != NULL) psfwidth_str >> lens->psf_width_x;
				if (lens != NULL) lens->psf_width_y = lens->psf_width_x;
			} else if (cardstring.find("ZSRC ") != string::npos) {
				string zsrc_string = cardstring.substr(11);
				stringstream zsrc_str;
				zsrc_str << zsrc_string;
				if (lens != NULL) {
					zsrc_str >> lens->source_redshift;
					if (lens->auto_zsource_scaling) lens->reference_source_redshift = lens->source_redshift;
				}
			} else if (cardstring.find("ZLENS ") != string::npos) {
				string zlens_string = cardstring.substr(11);
				stringstream zlens_str;
				zlens_str << zlens_string;
				if (lens != NULL) zlens_str >> lens->lens_redshift;
			}
		}
		if ((reading_markers) and (lens != NULL)) {
			// Commas are used in FITS file as delimeter so spaces don't get lost; now convert to spaces again
			for (size_t i = 0; i < lens->param_markers.size(); ++i) {
				 if (lens->param_markers[i] == ',') {
					  lens->param_markers.replace(i, 1, " ");
				 }
			}
		}

		bool new_dimensions = true;
		if (!fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status) )
		{
			if (naxis == 0) {
				die("Error: only 1D or 2D images are supported (dimension is %i)\n",naxis);
			} else {
				kk=0;
				long fpixel[naxis];
				for (kk=0; kk < naxis; kk++) fpixel[kk] = 1;

				if ((npixels_x != naxes[0]) or (npixels_y != naxes[1])) {
					if (in_mask != NULL) {
						for (i=0; i < npixels_x; i++) delete[] in_mask[i];
						delete[] in_mask;
					}
					if (extended_mask != NULL) {
						for (i=0; i < npixels_x; i++) delete[] extended_mask[i];
						delete[] extended_mask;
					}
					if (foreground_mask != NULL) {
						for (i=0; i < npixels_x; i++) delete[] foreground_mask[i];
						delete[] foreground_mask;
					}
					if ((QLens::setup_fft_convolution) and (lens != NULL)) lens->cleanup_FFT_convolution_arrays(); // since number of image pixels has changed, will need to redo FFT setup

					npixels_x = naxes[0];
					npixels_y = naxes[1];
					n_required_pixels = npixels_x*npixels_y;
				} else {
					new_dimensions = false;
				}

				n_high_sn_pixels = n_required_pixels; // this will be recalculated in assign_high_sn_pixels() function
				xvals = new double[npixels_x+1];
				yvals = new double[npixels_y+1];
				if (use_pixel_size) {
					if (!pixel_size_found) {
						if (lens==NULL) use_pixel_size = false; // couldn't find pixel size in file, and there is no lens object to find it from either
						else pixel_size = lens->data_pixel_size;
					}
				}
				if (use_pixel_size) {
					xstep = ystep = pixel_size;
					xmax = 0.5*npixels_x*pixel_size;
					ymax = 0.5*npixels_y*pixel_size;
					xmin=-xmax; ymin=-ymax;
				} else {
					xstep = (xmax-xmin)/npixels_x;
					ystep = (ymax-ymin)/npixels_y;
				}
				for (i=0, x=xmin; i <= npixels_x; i++, x += xstep) xvals[i] = x;
				for (i=0, y=ymin; i <= npixels_y; i++, y += ystep) yvals[i] = y;
				pixels = new double[npixels_x];
				surface_brightness = new double*[npixels_x];
				high_sn_pixel = new bool*[npixels_x];
				if (new_dimensions) {
					in_mask = new bool*[npixels_x];
					extended_mask = new bool*[npixels_x];
					foreground_mask = new bool*[npixels_x];
				}
				for (i=0; i < npixels_x; i++) {
					surface_brightness[i] = new double[npixels_y];
					high_sn_pixel[i] = new bool[npixels_y];
					if (new_dimensions) {
						in_mask[i] = new bool[npixels_y];
						extended_mask[i] = new bool[npixels_y];
						foreground_mask[i] = new bool[npixels_y];
						for (j=0; j < npixels_y; j++) {
							in_mask[i][j] = true;
							extended_mask[i][j] = true;
							foreground_mask[i][j] = true;
						}
					}
					for (j=0; j < npixels_y; j++) {
						high_sn_pixel[i][j] = true;
					}
				}

				for (fpixel[1]=1, j=0; fpixel[1] <= naxes[1]; fpixel[1]++, j++)
				{
					if (fits_read_pix(fptr, TDOUBLE, fpixel, naxes[0], NULL, pixels, NULL, &status) )  // read row of pixels
						break; // jump out of loop on error

					for (i=0; i < naxes[0]; i++) {
						surface_brightness[i][j] = pixels[i];
					}
				}
				delete[] pixels;
				image_load_status = true;
			}
		}
		fits_close_file(fptr, &status);

		pixel_xcvals = new double[npixels_x];
		pixel_ycvals = new double[npixels_y];
		for (i=0; i < npixels_x; i++) {
			pixel_xcvals[i] = (xvals[i]+xvals[i+1])/2;
		}
		for (i=0; i < npixels_y; i++) {
			pixel_ycvals[i] = (yvals[i]+yvals[i+1])/2;
		}
	}

	if (status) fits_report_error(stderr, status); // print any error message
	if (image_load_status) assign_high_sn_pixels();
	return image_load_status;
#endif
}

void ImagePixelData::save_data_fits(string fits_filename, const bool subimage, const double xmin_in, const double xmax_in, const double ymin_in, const double ymax_in)
{
#ifndef USE_FITS
	cout << "FITS capability disabled; QLens must be compiled with the CFITSIO library to write FITS files\n"; return;
#else
	int i,j,kk;
	fitsfile *outfptr;   // FITS file pointer, defined in fitsio.h
	int status = 0;   // CFITSIO status value MUST be initialized to zero!
	int bitpix = -64, naxis = 2;
	int min_i=0, min_j=0, max_i=npixels_x-1, max_j=npixels_y-1;
	if (subimage) {
		min_i = (int) ((xmin_in-xmin) * npixels_x / (xmax-xmin));
		if (min_i < 0) min_i = 0;
		max_i = (int) ((xmax_in-xmin) * npixels_x / (xmax-xmin));
		if (max_i > (npixels_x-1)) max_i = npixels_x-1;
		min_j = (int) ((ymin_in-ymin) * npixels_y / (ymax-ymin));
		if (min_j < 0) min_j = 0;
		max_j = (int) ((ymax_in-ymin) * npixels_y / (ymax-ymin));
		if (max_j > (npixels_y-1)) max_j = npixels_y-1;
	}
	int npix_x = max_i-min_i+1;
	int npix_y = max_j-min_j+1;
	cout << "imin=" << min_i << " imax=" << max_i << " jmin=" << min_j << " jmax=" << max_j << " npix_x=" << npix_x << " npix_y=" << npix_y << endl;
	long naxes[2] = {npix_x,npix_y};
	double *pixels;
	string fits_filename_overwrite = "!" + fits_filename; // ensures that it overwrites an existing file of the same name

	if (!fits_create_file(&outfptr, fits_filename_overwrite.c_str(), &status))
	{
		if (!fits_create_img(outfptr, bitpix, naxis, naxes, &status))
		{
			if (naxis == 0) {
				die("Error: only 1D or 2D images are supported (dimension is %i)\n",naxis);
			} else {
				kk=0;
				long fpixel[naxis];
				for (kk=0; kk < naxis; kk++) fpixel[kk] = 1;
				pixels = new double[npix_x];

				for (fpixel[1]=1, j=min_j; fpixel[1] <= naxes[1]; fpixel[1]++, j++)
				{
					for (i=min_i, kk=0; i <= max_i; i++, kk++) {
						pixels[kk] = surface_brightness[i][j];
					}
					fits_write_pix(outfptr, TDOUBLE, fpixel, naxes[0], pixels, &status);
				}
				delete[] pixels;
			}
			if (lens->data_pixel_size > 0)
				fits_write_key(outfptr, TDOUBLE, "PXSIZE", &lens->data_pixel_size, "length of square pixels (in arcsec)", &status);
			if (lens->data_pixel_noise != 0)
				fits_write_key(outfptr, TDOUBLE, "PXNOISE", &lens->data_pixel_noise, "pixel surface brightness noise", &status);
			if (lens->data_info != "") {
				string comment = "ql: " + lens->data_info;
				fits_write_comment(outfptr, comment.c_str(), &status);
			}
		}
		fits_close_file(outfptr, &status);
	} 

	if (status) fits_report_error(stderr, status); // print any error message
#endif
}

void ImagePixelData::get_grid_params(double& xmin_in, double& xmax_in, double& ymin_in, double& ymax_in, int& npx, int& npy)
{
	if (xvals==NULL) die("cannot get image pixel data parameters; no data has been loaded");
	xmin_in = xvals[0];
	xmax_in = xvals[npixels_x];
	ymin_in = yvals[0];
	ymax_in = yvals[npixels_y];
	npx = npixels_x;
	npy = npixels_y;
}

void ImagePixelData::assign_high_sn_pixels()
{
	global_max_sb = -1e30;
	int i,j;
	for (j=0; j < npixels_y; j++) {
		for (i=0; i < npixels_x; i++) {
			if (surface_brightness[i][j] > global_max_sb) global_max_sb = surface_brightness[i][j];
		}
	}
	if (lens != NULL) {
		n_high_sn_pixels = 0;
		for (j=0; j < npixels_y; j++) {
			for (i=0; i < npixels_x; i++) {
				if (surface_brightness[i][j] >= lens->high_sn_frac*global_max_sb) {
					high_sn_pixel[i][j] = true;
					n_high_sn_pixels++;
				}
				else high_sn_pixel[i][j] = false;
			}
		}
	}
}

double ImagePixelData::find_max_sb()
{
	double max_sb = -1e30;
	int i,j;
	for (j=0; j < npixels_y; j++) {
		for (i=0; i < npixels_x; i++) {
			if ((in_mask[i][j]) and (surface_brightness[i][j] > max_sb)) max_sb = surface_brightness[i][j];
		}
	}
	return max_sb;
}

double ImagePixelData::find_avg_sb(const double sb_threshold)
{
	double avg_sb=0;
	int npix=0;
	int i,j;
	for (j=0; j < npixels_y; j++) {
		for (i=0; i < npixels_x; i++) {
			if ((in_mask[i][j]) and (surface_brightness[i][j] > sb_threshold)) {
				avg_sb += surface_brightness[i][j];
				npix++;
			}
		}
	}
	avg_sb /= npix;


	return avg_sb;
}

bool ImagePixelData::load_mask_fits(string fits_filename, const bool foreground, const bool emask, const bool add_mask) // if 'add_mask' is true, then doesn't unmask any pixels that are already masked
{
#ifndef USE_FITS
	cout << "FITS capability disabled; QLens must be compiled with the CFITSIO library to read FITS files\n"; return false;
#else
	bool image_load_status = false;
	int i,j,kk;

	fitsfile *fptr;   // FITS file pointer, defined in fitsio.h
	int status = 0;   // CFITSIO status value MUST be initialized to zero!
	int bitpix, naxis;
	long naxes[2] = {1,1};
	double *pixels;
	int n_maskpixels = 0;

	if (!fits_open_file(&fptr, fits_filename.c_str(), READONLY, &status))
	{
		if (!fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status) )
		{
			if (naxis == 0) {
				die("Error: only 1D or 2D images are supported (dimension is %i)\n",naxis);
			} else {
				kk=0;
				long fpixel[naxis];
				for (kk=0; kk < naxis; kk++) fpixel[kk] = 1;
				if ((naxes[0] != npixels_x) or (naxes[1] != npixels_y)) { cout << "Error: number of pixels in mask file does not match number of pixels in loaded data\n"; return false; }
				pixels = new double[npixels_x];
				for (fpixel[1]=1, j=0; fpixel[1] <= naxes[1]; fpixel[1]++, j++)
				{
					if (fits_read_pix(fptr, TDOUBLE, fpixel, naxes[0], NULL, pixels, NULL, &status) )  // read row of pixels
						break; // jump out of loop on error

					for (i=0; i < naxes[0]; i++) {
						if (foreground) {
							if (pixels[i] == 0.0) {
								if (!add_mask) foreground_mask[i][j] = false;
								else if (foreground_mask[i][j]==true) n_maskpixels++;
							}
							else {
								foreground_mask[i][j] = true;
								n_maskpixels++;
								//cout << pixels[i] << endl;
							}
						} else if (emask) {
							if (pixels[i] == 0.0) {
								if (!add_mask) extended_mask[i][j] = false;
								else if (extended_mask[i][j]==true) n_maskpixels++;
							}
							else {
								extended_mask[i][j] = true;
								n_maskpixels++;
								//cout << pixels[i] << endl;
							}
						} else {
							if (pixels[i] == 0.0) {
								if (!add_mask) in_mask[i][j] = false;
								else if (in_mask[i][j]==true) n_maskpixels++;
							}
							else {
								in_mask[i][j] = true;
								if (!extended_mask[i][j]) extended_mask[i][j] = true; // the extended mask MUST contain all the primary mask pixels
								n_maskpixels++;
							//cout << pixels[i] << endl;
							}
						}
					}
				}
				delete[] pixels;
				image_load_status = true;
			}
		}
		fits_close_file(fptr, &status);
	} 

	if (status) fits_report_error(stderr, status); // print any error message
	if (!foreground) {
		if (image_load_status) n_required_pixels = n_maskpixels;
		//set_extended_mask(lens->extended_mask_n_neighbors);
	}
	return image_load_status;
#endif
}

void ImagePixelData::copy_mask(ImagePixelData* data)
{
	int i,j;
	if (data->npixels_x != npixels_x) die("cannot copy mask; different number of x-pixels (%i vs %i)",npixels_x,data->npixels_x);
	if (data->npixels_y != npixels_y) die("cannot copy mask; different number of y-pixels (%i vs %i)",npixels_y,data->npixels_y);
	for (j=0; j < npixels_y; j++) {
		for (i=0; i < npixels_x; i++) {
			in_mask[i][j] = data->in_mask[i][j];
		}
	}
	n_required_pixels = data->n_required_pixels;
}


bool ImagePixelData::save_mask_fits(string fits_filename, const bool foreground, const bool emask)
{
#ifndef USE_FITS
	cout << "FITS capability disabled; QLens must be compiled with the CFITSIO library to write FITS files\n"; return false;
#else
	int i,j,kk;
	fitsfile *outfptr;   // FITS file pointer, defined in fitsio.h
	int status = 0;   // CFITSIO status value MUST be initialized to zero!
	int bitpix = -64, naxis = 2;
	long naxes[2] = {npixels_x,npixels_y};
	double *pixels;
	string fits_filename_overwrite = "!" + fits_filename; // ensures that it overwrites an existing file of the same name

	if (!fits_create_file(&outfptr, fits_filename_overwrite.c_str(), &status))
	{
		if (!fits_create_img(outfptr, bitpix, naxis, naxes, &status))
		{
			if (naxis == 0) {
				die("Error: only 1D or 2D images are supported (dimension is %i)\n",naxis);
			} else {
				kk=0;
				long fpixel[naxis];
				for (kk=0; kk < naxis; kk++) fpixel[kk] = 1;
				pixels = new double[npixels_x];

				for (fpixel[1]=1, j=0; fpixel[1] <= naxes[1]; fpixel[1]++, j++)
				{
					for (i=0; i < npixels_x; i++) {
						if (foreground) {
							if (foreground_mask[i][j]) pixels[i] = 1.0;
							else pixels[i] = 0.0;
						} else if (emask) {
							if (extended_mask[i][j]) pixels[i] = 1.0;
							else pixels[i] = 0.0;
						} else {
							if (in_mask[i][j]) pixels[i] = 1.0;
							else pixels[i] = 0.0;
						}
					}
					fits_write_pix(outfptr, TDOUBLE, fpixel, naxes[0], pixels, &status);
				}
				delete[] pixels;
			}
		}
		fits_close_file(outfptr, &status);
	} 

	if (status) fits_report_error(stderr, status); // print any error message
	return true;
#endif
}

bool QLens::load_psf_fits(string fits_filename, const bool verbal)
{
#ifndef USE_FITS
	cout << "FITS capability disabled; QLens must be compiled with the CFITSIO library to read FITS files\n"; return false;
#else
	use_input_psf_matrix = true;
	bool image_load_status = false;
	int i,j,kk;
	if (psf_matrix != NULL) {
		for (i=0; i < psf_npixels_x; i++) delete[] psf_matrix[i];
		delete[] psf_matrix;
		psf_matrix = NULL;
	}
	double **input_psf_matrix;

	fitsfile *fptr;   // FITS file pointer, defined in fitsio.h
	int status = 0;   // CFITSIO status value MUST be initialized to zero!
	int bitpix, naxis;
	int nx, ny;
	long naxes[2] = {1,1};
	double *pixels;
	double peak_sb = -1e30;

	if (!fits_open_file(&fptr, fits_filename.c_str(), READONLY, &status))
	{
		if (!fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status) )
		{
			if (naxis == 0) {
				die("Error: only 1D or 2D images are supported (dimension is %i)\n",naxis);
			} else {
				kk=0;
				long fpixel[naxis];
				for (kk=0; kk < naxis; kk++) fpixel[kk] = 1;
				nx = naxes[0];
				ny = naxes[1];
				pixels = new double[nx];
				input_psf_matrix = new double*[nx];
				for (i=0; i < nx; i++) input_psf_matrix[i] = new double[ny];
				for (fpixel[1]=1, j=0; fpixel[1] <= naxes[1]; fpixel[1]++, j++)
				{
					if (fits_read_pix(fptr, TDOUBLE, fpixel, naxes[0], NULL, pixels, NULL, &status) )  // read row of pixels
						break; // jump out of loop on error

					for (i=0; i < naxes[0]; i++) {
						input_psf_matrix[i][j] = pixels[i];
						if (pixels[i] > peak_sb) peak_sb = pixels[i];
					}
				}
				delete[] pixels;
				image_load_status = true;
			}
		}
		fits_close_file(fptr, &status);
	} else {
		return false;
	}
	int imid, jmid, imin, imax, jmin, jmax;
	imid = nx/2;
	jmid = ny/2;
	imin = imid;
	imax = imid;
	jmin = jmid;
	jmax = jmid;
	for (i=0; i < nx; i++) {
		for (j=0; j < ny; j++) {
			if (input_psf_matrix[i][j] > psf_threshold*peak_sb) {
				if (i < imin) imin=i;
				if (i > imax) imax=i;
				if (j < jmin) jmin=j;
				if (j > jmax) jmax=j;
			}
		}
	}
	int nx_half, ny_half;
	nx_half = (imax-imin+1)/2;
	ny_half = (jmax-jmin+1)/2;
	psf_npixels_x = 2*nx_half+1;
	psf_npixels_y = 2*ny_half+1;
	psf_matrix = new double*[psf_npixels_x];
	for (i=0; i < psf_npixels_x; i++) psf_matrix[i] = new double[psf_npixels_y];
	int ii,jj;
	for (ii=0, i=imid-nx_half; ii < psf_npixels_x; i++, ii++) {
		for (jj=0, j=jmid-ny_half; jj < psf_npixels_y; j++, jj++) {
			psf_matrix[ii][jj] = input_psf_matrix[i][j];
		}
	}
	double normalization = 0;
	for (i=0; i < psf_npixels_x; i++) {
		for (j=0; j < psf_npixels_y; j++) {
			normalization += psf_matrix[i][j];
		}
	}
	for (i=0; i < psf_npixels_x; i++) {
		for (j=0; j < psf_npixels_y; j++) {
			psf_matrix[i][j] /= normalization;
		}
	}

	imid = nx/2;
	jmid = ny/2;
	imin = imid;
	imax = imid;
	jmin = jmid;
	jmax = jmid;
	for (i=0; i < nx; i++) {
		for (j=0; j < ny; j++) {
			if (input_psf_matrix[i][j] > foreground_psf_threshold*peak_sb) {
				if (i < imin) imin=i;
				if (i > imax) imax=i;
				if (j < jmin) jmin=j;
				if (j > jmax) jmax=j;
			}
		}
	}
	nx_half = (imax-imin+1)/2;
	ny_half = (jmax-jmin+1)/2;
	setup_foreground_PSF_matrix();
	//for (i=0; i < psf_npixels_x; i++) {
		//for (j=0; j < psf_npixels_y; j++) {
			//cout << psf_matrix[i][j] << " ";
		//}
		//cout << endl;
	//}
	//cout << psf_npixels_x << " " << psf_npixels_y << " " << nx_half << " " << ny_half << endl;

	if ((verbal) and (mpi_id==0)) {
		cout << "PSF matrix dimensions: " << psf_npixels_x << " " << psf_npixels_y << " (input PSF dimensions: " << nx << " " << ny << ")" << endl;
		cout << "Foreground PSF matrix dimensions: " << foreground_psf_npixels_x << " " << foreground_psf_npixels_y << endl;
		//cout << "PSF normalization =" << normalization << endl << endl;
	}
	for (i=0; i < nx; i++) delete[] input_psf_matrix[i];
	delete[] input_psf_matrix;

	if (status) fits_report_error(stderr, status); // print any error message
	return image_load_status;
#endif
}

void QLens::setup_foreground_PSF_matrix()
{
	// right now, we're just making foreground PSF same as regular PSF. Maybe later we'll allow for a custom PSF for the foreground galaxy?
	foreground_psf_npixels_x = psf_npixels_x;
	foreground_psf_npixels_y = psf_npixels_y;
	int nx_half = foreground_psf_npixels_x/2;
	int ny_half = foreground_psf_npixels_y/2;
	foreground_psf_matrix = new double*[foreground_psf_npixels_x];
	int i,j;
	for (i=0; i < foreground_psf_npixels_x; i++) foreground_psf_matrix[i] = new double[foreground_psf_npixels_y];
	for (i=0; i < foreground_psf_npixels_x; i++) {
		for (j=0; j < foreground_psf_npixels_y; j++) {
			foreground_psf_matrix[i][j] = psf_matrix[i][j];
		}
	}
	double normalization = 0;
	for (i=0; i < foreground_psf_npixels_x; i++) {
		for (j=0; j < foreground_psf_npixels_y; j++) {
			normalization += foreground_psf_matrix[i][j];
		}
	}
	for (i=0; i < psf_npixels_x; i++) {
		for (j=0; j < psf_npixels_y; j++) {
			foreground_psf_matrix[i][j] /= normalization;
		}
	}
}

ImagePixelData::~ImagePixelData()
{
	if (xvals != NULL) delete[] xvals;
	if (yvals != NULL) delete[] yvals;
	if (surface_brightness != NULL) {
		for (int i=0; i < npixels_x; i++) delete[] surface_brightness[i];
		delete[] surface_brightness;
	}
	if (high_sn_pixel != NULL) {
		for (int i=0; i < npixels_x; i++) delete[] high_sn_pixel[i];
		delete[] high_sn_pixel;
	}
	if (in_mask != NULL) {
		for (int i=0; i < npixels_x; i++) delete[] in_mask[i];
		delete[] in_mask;
	}
	if (extended_mask != NULL) {
		for (int i=0; i < npixels_x; i++) delete[] extended_mask[i];
		delete[] extended_mask;
	}
	if (foreground_mask != NULL) {
		for (int i=0; i < npixels_x; i++) delete[] foreground_mask[i];
		delete[] foreground_mask;
	}
}

void ImagePixelData::set_no_required_data_pixels()
{
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			in_mask[i][j] = false;
			extended_mask[i][j] = false;
		}
	}
	n_required_pixels = 0;
}

void ImagePixelData::set_all_required_data_pixels()
{
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			in_mask[i][j] = true;
			extended_mask[i][j] = true;
		}
	}
	n_required_pixels = npixels_x*npixels_y;
}

void ImagePixelData::set_foreground_mask_to_primary_mask()
{
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (in_mask[i][j]) foreground_mask[i][j] = true;
			else foreground_mask[i][j] = false;
		}
	}
}

void ImagePixelData::invert_mask()
{
	int i,j;
	n_required_pixels = 0;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (!in_mask[i][j]) {
				in_mask[i][j] = true;
				extended_mask[i][j] = true;
				n_required_pixels++;
			}
			else {
				in_mask[i][j] = false;
				extended_mask[i][j] = false;
			}
		}
	}
	n_required_pixels = npixels_x*npixels_y;
	find_extended_mask_rmax(); // used when splining integrals for deflection/hessian from Fourier modes
}

bool ImagePixelData::inside_mask(const double x, const double y)
{
	if ((x <= xmin) or (x >= xmax) or (y <= ymin) or (y >= ymax)) return false;
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if ((xvals[i] < x) and (xvals[i+1] >= x) and (yvals[j] < y) and (yvals[j+1] > y)) {
				if (in_mask[i][j]) return true;
			}
		}
	}
	return false;
}

void ImagePixelData::assign_mask_windows(const double sb_noise_threshold, const int threshold_size)
{
	vector<int> mask_window_sizes;
	vector<bool> active_mask_window;
	vector<double> mask_window_max_sb;
	int n_mask_windows = 0;
	int **mask_window_id = new int*[npixels_x];
	int i,j,l;
	for (i=0; i < npixels_x; i++) mask_window_id[i] = new int[npixels_y];
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			mask_window_id[i][j] = -1; // -1 means not belonging to a mask window
		}
	}

	int neighbor_mask, current_mask, this_window_id;
	bool new_mask_member;
	do {
		current_mask = -1;
		do {
			new_mask_member = false;
			for (j=0; j < npixels_y; j++) {
				for (l=0; l < 2*npixels_x; l++) {
					if (l < npixels_x) i=l;
					else i = 2*npixels_x-l-1;
					neighbor_mask = -1;
					if (in_mask[i][j]) {
						if ((current_mask == -1) and (mask_window_id[i][j] == -1)) {
							current_mask = n_mask_windows;
							mask_window_sizes.push_back(1);
							active_mask_window.push_back(true);
							mask_window_max_sb.push_back(surface_brightness[i][j]);
							mask_window_id[i][j] = n_mask_windows;
							n_mask_windows++;
							new_mask_member = true;
						} else {
							if (mask_window_id[i][j] == -1) {
								if ((i > 0) and (in_mask[i-1][j]) and (mask_window_id[i-1][j] == current_mask)) neighbor_mask = current_mask;
								else if ((i < npixels_x-1) and (in_mask[i+1][j]) and (mask_window_id[i+1][j] == current_mask)) neighbor_mask = current_mask;
								else if ((j > 0) and (in_mask[i][j-1]) and (mask_window_id[i][j-1] == current_mask)) neighbor_mask = current_mask;
								else if ((j < npixels_y-1) and (in_mask[i][j+1]) and (mask_window_id[i][j+1] == current_mask)) neighbor_mask = current_mask;
								if (neighbor_mask == current_mask) {
									mask_window_id[i][j] = neighbor_mask;
									mask_window_sizes[neighbor_mask]++;
									if (surface_brightness[i][j] > mask_window_max_sb[current_mask]) mask_window_max_sb[current_mask] = surface_brightness[i][j];
									new_mask_member = true;
								}
							}
							if (mask_window_id[i][j] == current_mask) {
								this_window_id = mask_window_id[i][j];
								if ((i > 0) and (in_mask[i-1][j]) and (mask_window_id[i-1][j] < 0)) {
									mask_window_id[i-1][j] = this_window_id;
									mask_window_sizes[this_window_id]++;
									if (surface_brightness[i-1][j] > mask_window_max_sb[current_mask]) mask_window_max_sb[current_mask] = surface_brightness[i-1][j];
									new_mask_member = true;
								}
								if ((i < npixels_x-1) and (in_mask[i+1][j]) and (mask_window_id[i+1][j] < 0)) {
									mask_window_id[i+1][j] = this_window_id;
									mask_window_sizes[this_window_id]++;
									if (surface_brightness[i+1][j] > mask_window_max_sb[current_mask]) mask_window_max_sb[current_mask] = surface_brightness[i+1][j];
									new_mask_member = true;
								}
								if ((j > 0) and (in_mask[i][j-1]) and (mask_window_id[i][j-1] < 0)) {
									mask_window_id[i][j-1] = this_window_id;
									mask_window_sizes[this_window_id]++;
									if (surface_brightness[i][j-1] > mask_window_max_sb[current_mask]) mask_window_max_sb[current_mask] = surface_brightness[i][j-1];
									new_mask_member = true;
								}
								if ((j < npixels_y-1) and (in_mask[i][j+1]) and (mask_window_id[i][j+1] < 0)) {
									mask_window_id[i][j+1] = this_window_id;
									mask_window_sizes[this_window_id]++;
									if (surface_brightness[i][j+1] > mask_window_max_sb[current_mask]) mask_window_max_sb[current_mask] = surface_brightness[i][j+1];
									new_mask_member = true;
								}
							}
						}
					}
				}
			}
		} while (new_mask_member);
	} while (current_mask != -1);
	int smallest_window_size, smallest_window_id;
	int n_windows_eff = n_mask_windows;
	int old_n_windows = n_windows_eff;
	do {
		old_n_windows = n_windows_eff;
		smallest_window_size = npixels_x*npixels_y;
		smallest_window_id = -1;
		for (l=0; l < n_mask_windows; l++) {
			if (active_mask_window[l]) {
				if ((mask_window_max_sb[l] > sb_noise_threshold*lens->data_pixel_noise) and (mask_window_sizes[l] > threshold_size)) {
					active_mask_window[l] = false; // ensures it won't get cut
				}
				else if ((mask_window_sizes[l] != 0) and (mask_window_sizes[l] < smallest_window_size)) {
					smallest_window_size = mask_window_sizes[l];
					smallest_window_id = l;
				}
			}
		}
		if ((smallest_window_id != -1) and (smallest_window_size > 0)) {
			for (i=0; i < npixels_x; i++) {
				for (j=0; j < npixels_y; j++) {
					if (mask_window_id[i][j]==smallest_window_id) {
						in_mask[i][j] = false;
					}
				}
			}
			active_mask_window[smallest_window_id] = false;
			mask_window_sizes[smallest_window_id] = 0;
			n_windows_eff--;
		}
	}
	while (n_windows_eff < old_n_windows);
	if (lens->mpi_id == 0) cout << "Trimmed " << n_mask_windows << " windows down to " << n_windows_eff << " windows" << endl;
	j=0;
	for (i=0; i < n_mask_windows; i++) {
		if (mask_window_sizes[i] != 0) {
			j++;
			if (lens->mpi_id == 0) cout << "Window " << j << " size: " << mask_window_sizes[i] << " max_sb: " << mask_window_max_sb[i] << endl;
		}
	}
	for (i=0; i < npixels_x; i++) delete[] mask_window_id[i];
	delete[] mask_window_id;
}

void ImagePixelData::unset_low_signal_pixels(const double sb_threshold, const bool use_fit)
{
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (use_fit) {
				if ((lens->image_pixel_grid->maps_to_source_pixel[i][j]) and (lens->image_pixel_grid->surface_brightness[i][j] < sb_threshold)) {
					if (in_mask[i][j]) {
						in_mask[i][j] = false;
						n_required_pixels--;
					}
				}
			} else {
				if (surface_brightness[i][j] < sb_threshold) {
					if (in_mask[i][j]) {
						in_mask[i][j] = false;
						n_required_pixels--;
					}
				}
			}
		}
	}

	// now we will deactivate pixels that have 0 or 1 neighboring active pixels (to get rid of isolated bits)
	bool **req = new bool*[npixels_x];
	for (i=0; i < npixels_x; i++) req[i] = new bool[npixels_y];
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			req[i][j] = in_mask[i][j];
		}
	}
	int n_active_neighbors;
	for (int k=0; k < 3; k++) {
		for (i=0; i < npixels_x; i++) {
			for (j=0; j < npixels_y; j++) {
				if (in_mask[i][j]) {
					n_active_neighbors = 0;
					if ((i < npixels_x-1) and (in_mask[i+1][j])) n_active_neighbors++;
					if ((i > 0) and (in_mask[i-1][j])) n_active_neighbors++;
					if ((j < npixels_y-1) and (in_mask[i][j+1])) n_active_neighbors++;
					if ((j > 0) and (in_mask[i][j-1])) n_active_neighbors++;
					if ((n_active_neighbors < 2) and (req[i][j])) {
						req[i][j] = false;
						n_required_pixels--;
					}
				}
			}
		}
		for (i=0; i < npixels_x; i++) {
			for (j=0; j < npixels_y; j++) {
				in_mask[i][j] = req[i][j];
			}
		}
	}
	// check for any lingering "holes" in the mask and activate them
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (!in_mask[i][j]) {
				if (((i < npixels_x-1) and (in_mask[i+1][j])) and ((i > 0) and (in_mask[i-1][j])) and ((j < npixels_y-1) and (in_mask[i][j+1])) and ((j > 0) and (in_mask[i][j-1]))) {
					if (!req[i][j]) {
						in_mask[i][j] = true;
						n_required_pixels++;
					}
				}
			}
		}
	}

	for (i=0; i < npixels_x; i++) delete[] req[i];
	delete[] req;
}

void ImagePixelData::set_positive_radial_gradient_pixels()
{
	int i,j;
	lensvector rhat, grad;
	double rderiv;
	for (i=0; i < npixels_x-1; i++) {
		for (j=0; j < npixels_y-1; j++) {
			if (!in_mask[i][j]) {
				rhat[0] = pixel_xcvals[i];
				rhat[1] = pixel_ycvals[j];
				rhat /= rhat.norm();
				grad[0] = surface_brightness[i+1][j] - surface_brightness[i][j];
				grad[1] = surface_brightness[i][j+1] - surface_brightness[i][j];
				rderiv = grad * rhat; // dot product
				if (rderiv > 0) {
					in_mask[i][j] = true;
					n_required_pixels++;
				}
			}
		}
	}
}

void ImagePixelData::set_neighbor_pixels(const bool only_interior_neighbors, const bool only_exterior_neighbors)
{
	int i,j;
	double r0, r;
	bool **req = new bool*[npixels_x];
	for (i=0; i < npixels_x; i++) req[i] = new bool[npixels_y];
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			req[i][j] = in_mask[i][j];
		}
	}
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if ((in_mask[i][j])) {
				if ((only_interior_neighbors) or (only_exterior_neighbors)) r0 = sqrt(SQR(pixel_xcvals[i]) + SQR(pixel_ycvals[j]));
				if ((i < npixels_x-1) and (!in_mask[i+1][j])) {
					if (!req[i+1][j]) {
						if ((only_interior_neighbors) or (only_exterior_neighbors)) r = sqrt(SQR(pixel_xcvals[i+1]) + SQR(pixel_ycvals[j]));
						if (((only_interior_neighbors) and (r > r0)) or ((only_exterior_neighbors) and (r < r0))) ;
						else {
							req[i+1][j] = true;
							n_required_pixels++;
						}
					}
				}
				if ((i > 0) and (!in_mask[i-1][j])) {
					if (!req[i-1][j]) {
						if ((only_interior_neighbors) or (only_exterior_neighbors)) r = sqrt(SQR(pixel_xcvals[i-1]) + SQR(pixel_ycvals[j]));
						if (((only_interior_neighbors) and (r > r0)) or ((only_exterior_neighbors) and (r < r0))) ;
						else {
							req[i-1][j] = true;
							n_required_pixels++;
						}
					}
				}
				if ((j < npixels_y-1) and (!in_mask[i][j+1])) {
					if (!req[i][j+1]) {
						if ((only_interior_neighbors) or (only_exterior_neighbors)) r = sqrt(SQR(pixel_xcvals[i]) + SQR(pixel_ycvals[j+1]));
						if (((only_interior_neighbors) and (r > r0)) or ((only_exterior_neighbors) and (r < r0))) ;
						else {
							req[i][j+1] = true;
							n_required_pixels++;
						}
					}
				}
				if ((j > 0) and (!in_mask[i][j-1])) {
					if (!req[i][j-1]) {
						if ((only_interior_neighbors) or (only_exterior_neighbors)) r = sqrt(SQR(pixel_xcvals[i]) + SQR(pixel_ycvals[j-1]));
						if (((only_interior_neighbors) and (r > r0)) or ((only_exterior_neighbors) and (r < r0))) ;
						else {
							req[i][j-1] = true;
							n_required_pixels++;
						}
					}
				}
			}
		}
	}
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			in_mask[i][j] = req[i][j];
		}
	}
	// check for any lingering "holes" in the mask and activate them
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (!in_mask[i][j]) {
				if (((i < npixels_x-1) and (in_mask[i+1][j])) and ((i > 0) and (in_mask[i-1][j])) and ((j < npixels_y-1) and (in_mask[i][j+1])) and ((j > 0) and (in_mask[i][j-1]))) {
					if (!req[i][j]) {
						req[i][j] = true;
						n_required_pixels++;
					}
				}
			}
		}
	}
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			in_mask[i][j] = req[i][j];
		}
	}
	for (i=0; i < npixels_x; i++) delete[] req[i];
	delete[] req;
}

void ImagePixelData::set_required_data_window(const double xmin, const double xmax, const double ymin, const double ymax, const bool unset)
{
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if ((xvals[i] > xmin) and (xvals[i+1] < xmax) and (yvals[j] > ymin) and (yvals[j+1] < ymax)) {
				if (!unset) {
					if (in_mask[i][j] == false) {
						in_mask[i][j] = true;
						n_required_pixels++;
					}
				} else {
					if (in_mask[i][j] == true) {
						in_mask[i][j] = false;
						n_required_pixels--;
					}
				}
			}
		}
	}
}

void ImagePixelData::set_required_data_annulus(const double xc, const double yc, const double rmin, const double rmax, double theta1_deg, double theta2_deg, const double xstretch, const double ystretch, const bool unset)
{
	// the angles MUST be between 0 and 360 here, so we enforce this in the following
	while (theta1_deg < 0) theta1_deg += 360;
	while (theta1_deg > 360) theta1_deg -= 360;
	while (theta2_deg < 0) theta2_deg += 360;
	while (theta2_deg > 360) theta2_deg -= 360;
	double x, y, rsq, rminsq, rmaxsq, theta, theta1, theta2;
	rminsq = rmin*rmin;
	rmaxsq = rmax*rmax;
	theta1 = degrees_to_radians(theta1_deg);
	theta2 = degrees_to_radians(theta2_deg);
	int i,j;
	double theta_old;
	for (i=0; i < npixels_x; i++) {
		x = 0.5*(xvals[i] + xvals[i+1]);
		for (j=0; j < npixels_y; j++) {
			y = 0.5*(yvals[j] + yvals[j+1]);
			rsq = SQR((x-xc)/xstretch) + SQR((y-yc)/ystretch);
			theta = atan(abs(((y-yc)/(x-xc))*xstretch/ystretch));
			theta_old=theta;
			if (x < xc) {
				if (y < yc)
					theta = theta + M_PI;
				else
					theta = M_PI - theta;
			} else if (y < yc) {
				theta = M_2PI - theta;
			}
			if ((rsq > rminsq) and (rsq < rmaxsq)) {
				// allow for two possibilities: theta1 < theta2, and theta2 < theta1 (which can happen if, e.g. theta2 is input as negative and theta1 is input as positive)
				if (((theta2 > theta1) and (theta >= theta1) and (theta <= theta2)) or ((theta1 > theta2) and ((theta >= theta1) or (theta <= theta2)))) {
					if (!unset) {
						if (in_mask[i][j] == false) {
							in_mask[i][j] = true;
							n_required_pixels++;
						}
					} else {
						if (in_mask[i][j] == true) {
							in_mask[i][j] = false;
							n_required_pixels--;
						}
					}
				}
			}
		}
	}
}

void ImagePixelData::reset_extended_mask()
{
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			extended_mask[i][j] = in_mask[i][j];
		}
	}
	find_extended_mask_rmax(); // used when splining integrals for deflection/hessian from Fourier modes
}

void ImagePixelData::set_extended_mask(const int n_neighbors, const bool add_to_emask_in, const bool only_interior_neighbors)
{
	// This is very similar to the set_neighbor_pixels() function in ImagePixelData; used here for the outside_sb_prior feature
	int i,j,k;
	bool add_to_emask = add_to_emask_in;
	if (n_neighbors < 0) {
		for (i=0; i < npixels_x; i++) {
			for (j=0; j < npixels_y; j++) {
				if (foreground_mask[i][j]) extended_mask[i][j] = true;
			}
		}
		return;
	}
	if ((add_to_emask) and (get_size_of_extended_mask()==0)) add_to_emask = false;
	bool **req = new bool*[npixels_x];
	for (i=0; i < npixels_x; i++) req[i] = new bool[npixels_y];
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (!add_to_emask) {
				extended_mask[i][j] = in_mask[i][j];
				req[i][j] = in_mask[i][j];
			} else {
				req[i][j] = extended_mask[i][j];
			}
		}
	}
	double r0=0, r;
	int emask_npix, emask_npix0;
	for (k=0; k < n_neighbors; k++) {
		emask_npix0 = get_size_of_extended_mask();
		for (i=0; i < npixels_x; i++) {
			for (j=0; j < npixels_y; j++) {
				if (req[i][j]) {
					if (only_interior_neighbors) r0 = sqrt(SQR(pixel_xcvals[i]) + SQR(pixel_ycvals[j]));
					if ((i < npixels_x-1) and (!req[i+1][j])) {
						if (!extended_mask[i+1][j]) {
							if (only_interior_neighbors) r = sqrt(SQR(pixel_xcvals[i+1]) + SQR(pixel_ycvals[j]));
							if ((only_interior_neighbors) and ((r - r0) > 1e-6)) {
								//cout << "NOT adding pixel " << (i+1) << " " << j << " " << r << " vs " << r0 << endl;
							}
							else {
								if (foreground_mask[i+1][j]) extended_mask[i+1][j] = true;
								//cout << "Adding pixel " << (i+1) << " " << j << " " << r0 << endl;
							}
						}
					}
					if ((i > 0) and (!req[i-1][j])) {
						if (!extended_mask[i-1][j]) {
							if (only_interior_neighbors) r = sqrt(SQR(pixel_xcvals[i-1]) + SQR(pixel_ycvals[j]));
							if ((only_interior_neighbors) and ((r - r0) > 1e-6)) {
								//cout << "NOT Adding pixel " << (i-1) << " " << j << " " << r << " vs " << r0 << endl;
							} else {
								if (foreground_mask[i-1][j]) extended_mask[i-1][j] = true;
								//cout << "Adding pixel " << (i-1) << " " << j << " " << r << " vs " << r0 << endl;
							}
						}
					}
					if ((j < npixels_y-1) and (!req[i][j+1])) {
						if (!extended_mask[i][j+1]) {
							if (only_interior_neighbors) r = sqrt(SQR(pixel_xcvals[i]) + SQR(pixel_ycvals[j+1]));
							if ((only_interior_neighbors) and ((r - r0) > 1e-6)) {
								//cout << "NOT Adding pixel " << (i) << " " << (j+1) << " " << r << " vs " << r0 << endl;
							} else {
								if (foreground_mask[i][j+1]) extended_mask[i][j+1] = true;
								//cout << "Adding pixel " << (i) << " " << (j+1) << " " << r << " vs " << r0 << endl;
							}
						}
					}
					if ((j > 0) and (!req[i][j-1])) {
						if (!extended_mask[i][j-1]) {
							if (only_interior_neighbors) r = sqrt(SQR(pixel_xcvals[i]) + SQR(pixel_ycvals[j-1]));
							if ((only_interior_neighbors) and ((r - r0) > 1e-6)) {
								//cout << "NOT Adding pixel " << (i) << " " << (j-1) << " " << r << " vs " << r0 << endl;
							} else {
								if (foreground_mask[i][j-1]) extended_mask[i][j-1] = true;
								//cout << "Adding pixel " << (i) << " " << (j-1) << " " << r << " vs " << r0 << endl;
							}
						}
					}
				}
			}
		}
		// check for any lingering "holes" in the mask and activate them
		for (i=0; i < npixels_x; i++) {
			for (j=0; j < npixels_y; j++) {
				if (!extended_mask[i][j]) {
					if (((i < npixels_x-1) and (extended_mask[i+1][j])) and ((i > 0) and (extended_mask[i-1][j])) and ((j < npixels_y-1) and (extended_mask[i][j+1])) and ((j > 0) and (extended_mask[i][j-1]))) {
						if (!extended_mask[i][j]) {
							if (foreground_mask[i][j]) extended_mask[i][j] = true;
							//cout << "Filling hole " << i << " " << j << endl;
						}
					}
				}
			}
		}
		for (i=0; i < npixels_x; i++) {
			for (j=0; j < npixels_y; j++) {
				req[i][j] = extended_mask[i][j];
			}
		}
		emask_npix = get_size_of_extended_mask();
		if (emask_npix==emask_npix0) break;

		//long int npix = 0;
		//for (i=0; i < npixels_x; i++) {
			//for (j=0; j < npixels_y; j++) {
				//if (extended_mask[i][j]) npix++;
			//}
		//}
		//cout << "iteration " << k << ": npix=" << npix << endl;
	}
	find_extended_mask_rmax();
	for (i=0; i < npixels_x; i++) delete[] req[i];
	delete[] req;
}

void ImagePixelData::activate_partner_image_pixels()
{
	int i,j,k,ii,jj,n_images;
	bool found_itself;
	double xstep = xvals[1] - xvals[0];
	double ystep = yvals[1] - yvals[0];
	double rsq, outermost_rsq = 0, innermost_rsq = 100000;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			found_itself = false;
			if (extended_mask[i][j]) {
				rsq = SQR(pixel_xcvals[i]) + SQR(pixel_ycvals[i]);
				if (rsq > outermost_rsq) outermost_rsq = rsq;
				if (rsq < innermost_rsq) innermost_rsq = rsq;
				lensvector pos,src;
				pos[0] = pixel_xcvals[i];
				pos[1] = pixel_ycvals[j];
				lens->find_sourcept(pos,src,0,lens->reference_zfactors,lens->default_zsrc_beta_factors);
				image *img = lens->get_images(src, n_images, false);
				for (k=0; k < n_images; k++) {
					if ((img[k].mag > 0) and (abs(img[k].mag) < 0.1)) continue; // ignore central images
					ii = (int) ((img[k].pos[0] - xvals[0]) / xstep);
					jj = (int) ((img[k].pos[1] - yvals[0]) / ystep);
					if ((ii < 0) or (jj < 0) or (ii > npixels_x) or (jj > npixels_y)) continue;
					if ((ii==i) and (jj==j)) {
						found_itself = true;
						continue;
					}
					if ((!extended_mask[ii][jj]) and (foreground_mask[ii][jj])) { // any pixels that are not in the foreground mask shouldn't be in emask either
						extended_mask[ii][jj] = true;
						rsq = SQR(pixel_xcvals[i]) + SQR(pixel_ycvals[i]);
						if (rsq > outermost_rsq) outermost_rsq = rsq;
						if (rsq < innermost_rsq) innermost_rsq = rsq;
					}
				}
				if (found_itself) cout << "Found itself! Yay!" << endl;
				else cout << "NOTE: pixel couldn't find itself" << endl;
			}
		}
	}

	// Now, we check pixels still not in the extended mask and see if any of them have partner images inside the extended mask; if so, activate it
	outermost_rsq = SQR(sqrt(outermost_rsq) + 0.05);
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (!foreground_mask[i][j]) continue;
			rsq = SQR(pixel_xcvals[i]) + SQR(pixel_ycvals[i]);
			if ((rsq < outermost_rsq) and (rsq > innermost_rsq) and (!extended_mask[i][j])) {
				lensvector pos,src;
				pos[0] = pixel_xcvals[i];
				pos[1] = pixel_ycvals[j];
				lens->find_sourcept(pos,src,0,lens->reference_zfactors,lens->default_zsrc_beta_factors);
				image *img = lens->get_images(src, n_images, false);
				for (k=0; k < n_images; k++) {
					if ((img[k].mag > 0) and (abs(img[k].mag) < 0.1)) continue; // ignore central images
					ii = (int) ((img[k].pos[0] - xvals[0]) / xstep);
					jj = (int) ((img[k].pos[1] - yvals[0]) / ystep);
					if ((ii < 0) or (jj < 0) or (ii > npixels_x) or (jj > npixels_y)) continue;
					if ((ii==i) and (jj==j)) continue;
					if (extended_mask[ii][jj]) extended_mask[i][j] = true;
				}
			}
		}
	}

	// check for any lingering "holes" in the mask and activate them
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (!extended_mask[i][j]) {
				if (((i < npixels_x-1) and (extended_mask[i+1][j])) and ((i > 0) and (extended_mask[i-1][j])) and ((j < npixels_y-1) and (extended_mask[i][j+1])) and ((j > 0) and (extended_mask[i][j-1]))) {
					if (!extended_mask[i][j]) {
						extended_mask[i][j] = true;
						//cout << "Filling hole " << i << " " << j << endl;
					}
				}
			}
		}
	}

}

void ImagePixelData::set_extended_mask_annulus(const double xc, const double yc, const double rmin, const double rmax, double theta1_deg, double theta2_deg, const double xstretch, const double ystretch, const bool unset)
{
	// the angles MUST be between 0 and 360 here, so we enforce this in the following
	while (theta1_deg < 0) theta1_deg += 360;
	while (theta1_deg > 360) theta1_deg -= 360;
	while (theta2_deg < 0) theta2_deg += 360;
	while (theta2_deg > 360) theta2_deg -= 360;
	double x, y, rsq, rminsq, rmaxsq, theta, theta1, theta2;
	rminsq = rmin*rmin;
	rmaxsq = rmax*rmax;
	theta1 = degrees_to_radians(theta1_deg);
	theta2 = degrees_to_radians(theta2_deg);
	int i,j;
	double theta_old;
	bool pixels_in_mask = false;
	for (i=0; i < npixels_x; i++) {
		x = 0.5*(xvals[i] + xvals[i+1]);
		for (j=0; j < npixels_y; j++) {
			y = 0.5*(yvals[j] + yvals[j+1]);
			rsq = SQR((x-xc)/xstretch) + SQR((y-yc)/ystretch);
			theta = atan(abs(((y-yc)/(x-xc))*xstretch/ystretch));
			theta_old=theta;
			if (x < xc) {
				if (y < yc)
					theta = theta + M_PI;
				else
					theta = M_PI - theta;
			} else if (y < yc) {
				theta = M_2PI - theta;
			}
			if ((rsq > rminsq) and (rsq < rmaxsq)) {
				// allow for two possibilities: theta1 < theta2, and theta2 < theta1 (which can happen if, e.g. theta1 is input as negative and theta1 is input as positive)
				if (((theta2 > theta1) and (theta >= theta1) and (theta <= theta2)) or ((theta1 > theta2) and ((theta >= theta1) or (theta <= theta2)))) {
					if (!unset) {
						if (extended_mask[i][j] == false) {
							extended_mask[i][j] = true;
						}
					} else {
						if (extended_mask[i][j] == true) {
							if (!in_mask[i][j]) extended_mask[i][j] = false;
							else pixels_in_mask = true;
						}
					}
				}
			}
		}
	}
	find_extended_mask_rmax();
	if (pixels_in_mask) warn("some pixels in the annulus were in the primary (lensed image) mask, and therefore could not be removed from extended mask");
}

void ImagePixelData::find_extended_mask_rmax()
{
	double r, rmax = -1e30;
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (extended_mask[i][j]) {
				r = sqrt(SQR(pixel_xcvals[i]) + SQR(pixel_ycvals[j+1]));
				if (r > rmax) rmax = r;
			}
		}
	}
	emask_rmax = rmax;
	return;
}


void ImagePixelData::set_foreground_mask_annulus(const double xc, const double yc, const double rmin, const double rmax, double theta1_deg, double theta2_deg, const double xstretch, const double ystretch, const bool unset)
{
	// the angles MUST be between 0 and 360 here, so we enforce this in the following
	while (theta1_deg < 0) theta1_deg += 360;
	while (theta1_deg > 360) theta1_deg -= 360;
	while (theta2_deg < 0) theta2_deg += 360;
	while (theta2_deg > 360) theta2_deg -= 360;
	double x, y, rsq, rminsq, rmaxsq, theta, theta1, theta2;
	rminsq = rmin*rmin;
	rmaxsq = rmax*rmax;
	theta1 = degrees_to_radians(theta1_deg);
	theta2 = degrees_to_radians(theta2_deg);
	int i,j;
	double theta_old;
	bool pixels_in_mask = false;
	for (i=0; i < npixels_x; i++) {
		x = 0.5*(xvals[i] + xvals[i+1]);
		for (j=0; j < npixels_y; j++) {
			y = 0.5*(yvals[j] + yvals[j+1]);
			rsq = SQR((x-xc)/xstretch) + SQR((y-yc)/ystretch);
			theta = atan(abs(((y-yc)/(x-xc))*xstretch/ystretch));
			theta_old=theta;
			if (x < xc) {
				if (y < yc)
					theta = theta + M_PI;
				else
					theta = M_PI - theta;
			} else if (y < yc) {
				theta = M_2PI - theta;
			}
			if ((rsq > rminsq) and (rsq < rmaxsq)) {
				// allow for two possibilities: theta1 < theta2, and theta2 < theta1 (which can happen if, e.g. theta1 is input as negative and theta1 is input as positive)
				if (((theta2 > theta1) and (theta >= theta1) and (theta <= theta2)) or ((theta1 > theta2) and ((theta >= theta1) or (theta <= theta2)))) {
					if (!unset) {
						if (foreground_mask[i][j] == false) {
							foreground_mask[i][j] = true;
						}
					} else {
						if (foreground_mask[i][j] == true) {
							if (!in_mask[i][j]) foreground_mask[i][j] = false;
							else pixels_in_mask = true;
						}
					}
				}
			}
		}
	}
	if (pixels_in_mask) warn("some pixels in the annulus were in the primary (lensed image) mask, and therefore could not be removed from foreground mask");
}

long int ImagePixelData::get_size_of_extended_mask()
{
	int i,j;
	long int npix = 0;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (extended_mask[i][j]) npix++;
		}
	}
	return npix;
}

long int ImagePixelData::get_size_of_foreground_mask()
{
	int i,j;
	long int npix = 0;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if (foreground_mask[i][j]) npix++;
		}
	}
	return npix;
}



void ImagePixelData::estimate_pixel_noise(const double xmin, const double xmax, const double ymin, const double ymax, double &noise, double &mean_sb)
{
	int i,j;
	int imin=0, imax=npixels_x-1, jmin=0, jmax=npixels_y-1;
	bool passed_min=false;
	for (i=0; i < npixels_x; i++) {
		if ((passed_min==false) and ((xvals[i+1]+xvals[i]) > 2*xmin)) {
			imin = i;
			passed_min = true;
		} else if (passed_min==true) {
			if ((xvals[i+1]+xvals[i]) > 2*xmax) {
				imax = i-1;
				break;
			}
		}
	}
	passed_min = false;
	for (j=0; j < npixels_y; j++) {
		if ((passed_min==false) and ((yvals[j+1]+yvals[j]) > 2*ymin)) {
			jmin = j;
			passed_min = true;
		} else if (passed_min==true) {
			if ((yvals[j+1]+yvals[j]) > 2*ymax) {
				jmax = j-1;
				break;
			}
		}
	}
	if ((imin==imax) or (jmin==jmax)) die("window for centroid calculation has zero size");
	double sigsq_sb=0, total_flux=0;
	int np=0;
	double xm,ym;
	for (j=jmin; j <= jmax; j++) {
		for (i=imin; i <= imax; i++) {
			if (in_mask[i][j]) {
				total_flux += surface_brightness[i][j];
				np++;
			}
		}
	}

	mean_sb = total_flux / np;
	for (j=jmin; j <= jmax; j++) {
		for (i=imin; i <= imax; i++) {
			if (in_mask[i][j]) {
				sigsq_sb += SQR(surface_brightness[i][j]-mean_sb);
			}
		}
	}
	double sqrnoise = sigsq_sb/np;
	double sigthreshold = 3.0;
	double sqrthreshold = SQR(sigthreshold)*sqrnoise;
	noise = sqrt(sqrnoise);
	int nclip=0, prev_nclip;
	double difsqr;
	do {
		prev_nclip = nclip;
		nclip = 0;
		sigsq_sb = 0;
		np = 0;
		total_flux = 0;
		for (j=jmin; j <= jmax; j++) {
			for (i=imin; i <= imax; i++) {
				if (in_mask[i][j]) {
					difsqr = SQR(surface_brightness[i][j]-mean_sb);
					if (difsqr > sqrthreshold) nclip++;
					else {
						sigsq_sb += difsqr;
						total_flux += surface_brightness[i][j];
						np++;
					}
				}
			}
		}
		sqrnoise = sigsq_sb/np;
		sqrthreshold = SQR(sigthreshold)*sqrnoise;
		noise = sqrt(sqrnoise);
		mean_sb = total_flux / np;
	} while (nclip > prev_nclip);
}

bool ImagePixelData::fit_isophote(const double xi0, const double xistep, const int emode, const double qi, const double theta_i_degrees, const double xc_i, const double yc_i, const int max_it, IsophoteData &isophote_data, const bool use_polar_higher_harmonics, const bool verbose, SB_Profile* sbprofile, const int default_sampling_mode_in, const int n_higher_harmonics, const bool fix_center, const int max_xi_it, const double ximax_in, const double rms_sbgrad_rel_threshold, const double npts_frac, const double rms_sbgrad_rel_transition, const double npts_frac_zeroweight)
{
	const int npts_max = 2000;
	const int min_it = 7;
	const double sbfrac = 0.04;
	int default_sampling_mode = default_sampling_mode_in;

	if (max_it < min_it) {
		warn("cannot have less than %i max iterations for isophote fit",min_it);
		return false;
	}

	ofstream ellout;
	if (verbose) ellout.open("ellfit.dat"); // only make the plot if "verbose" is set to true

	if (pixel_size <= 0) {
		pixel_size = dmax(pixel_xcvals[1]-pixel_xcvals[0],pixel_ycvals[1]-pixel_ycvals[0]);
	}
	//double xi_min = 2.5*pixel_size;
	double xi_min = 1.5*pixel_size;
	double xi_max = dmax(pixel_xcvals[npixels_x-1],pixel_ycvals[npixels_y-1]); // NOTE: we may never reach xi_max if S/N falls too low
	if (ximax_in > 0) {
		if (ximax_in > xi_max) {
			warn("cannot do isophote fit with xi_max greater than extend of pixel image");
			return false;
		} else {
			xi_max = ximax_in;
		}
	}
	double xi=xi0;
	double xifac = 1+xistep;
	int i, i_switch, j, k;
	for (i=0, xi=xi0; xi < xi_max; i++, xi *= xifac) ;
	i_switch = i;
	for (xi=xi0/xifac; xi > xi_min; i++, xi /= xifac) ;
	int n_xivals = i;
	if (n_xivals > max_xi_it) n_xivals = max_xi_it;

	int *xi_ivals = new int[n_xivals];
	int *xi_ivals_sorted = new int[n_xivals];
	double *xivals = new double[n_xivals];
	bool *repeat_params = new bool[n_xivals];
	for (i=0; i < n_xivals; i++) repeat_params[i] = false;

	for (i=0, xi=xi0; xi < xi_max; i++, xi *= xifac) { xivals[i] = xi; xi_ivals_sorted[i] = i; if (i==n_xivals-1) { i++; break;} }
	i_switch = i;
	if (i < n_xivals) {
		for (xi=xi0/xifac; xi > xi_min; i++, xi /= xifac) { xivals[i] = xi; xi_ivals_sorted[i] = i; if (i==n_xivals-1) { i++; break;} }
	}
	if (n_xivals > 1) sort(n_xivals,xivals,xi_ivals_sorted);
	for (i=0; i < n_xivals; i++) {
		for (j=0; j < n_xivals; j++) {
			if (xi_ivals_sorted[i] == j) {
				xi_ivals[j] = i;
				break;
			}
		}
	}
	isophote_data.input(n_xivals,xivals);
	isophote_data.setnan(); // in case any of the isophotes can't be fit, the NAN will indicate it
	if (n_higher_harmonics > 2) isophote_data.use_A56 = true;

	/*************************************** lambda functions ******************************************/
	// We'll use this lambda function to construct the matrices used for least-squares fitting
	auto fill_matrices = [](const int npts, const int nmax_amp, double *sb_residual, double *sb_weights, double **smatrix, double *Dvec, double **Smatrix, const double noise)
	{
		int i,j,k;
		double sqrnoise = noise*noise;
		for (i=0; i < nmax_amp; i++) {
			if ((sb_residual != NULL) and (Dvec != NULL)) {
				Dvec[i] = 0;
				for (k=0; k < npts; k++) {
					Dvec[i] += smatrix[i][k]*sb_residual[k]/sqrnoise;
					//if (sb_weights==NULL) Dvec[i] += smatrix[i][k]*sb_residual[k]/sqrnoise;
					//else Dvec[i] += smatrix[i][k]*sb_residual[k]*sb_weights[k]/sqrnoise;
				}
			}
			for (j=0; j <= i; j++) {
				Smatrix[i][j] = 0;
				for (k=0; k < npts; k++) {
					Smatrix[i][j] += smatrix[i][k]*smatrix[j][k]/sqrnoise;
					//if (sb_weights==NULL) Smatrix[i][j] += smatrix[i][k]*smatrix[j][k]/sqrnoise;
					//else Smatrix[i][j] += smatrix[i][k]*smatrix[j][k]*sb_weights[k]/sqrnoise;
				}
			}
		}
	};

	auto find_sbgrad = [](const int npts_sample, double *sbvals_prev, double *sbvals_next, double *sbgrad_weights_prev, double *sbgrad_weights_next, const double &gradstep, double &sb_grad, double &rms_sbgrad_rel, int &ngrad)
	{
		sb_grad=0;
		int i;
		ngrad=0;
		double sb_grad_sq=0, denom=0, sbgrad_i, sbgrad_wgt;
		for (i=0; i < npts_sample; i++) {
			if (!std::isnan(sbvals_prev[i]) and (!std::isnan(sbvals_next[i]))) {
				sbgrad_wgt = sbgrad_weights_prev[i]+sbgrad_weights_next[i]; // adding uncertainties in quadrature
				sbgrad_i = (sbvals_next[i] - sbvals_prev[i])/gradstep;
				//sb_grad += sbgrad_i;
				//sb_grad_sq += sbgrad_i*sbgrad_i;
				//denom += 1.0;
				sb_grad += sbgrad_i / sbgrad_wgt;
				sb_grad_sq += sbgrad_i*sbgrad_i / sbgrad_wgt;
				denom += 1.0/sbgrad_wgt;
				ngrad++;
			}
		}
		if (ngrad==0) {
			warn("no sampling points could be used for SB gradiant");
			return false;
		}
		sb_grad /= denom;
		sb_grad_sq /= denom;
		rms_sbgrad_rel = sqrt(sb_grad_sq - sb_grad*sb_grad)/abs(sb_grad);
		return true;
	};
	/***************************************************************************************************/

	double epsilon0, theta0, xc0, yc0; // these will be the best-fit params at the first radius xi0, since we'll return to it and step to smaller xi later
	double epsilon_prev, theta_prev, xc_prev, yc_prev; 
	double xc_err, yc_err, epsilon_err, theta_err;
	double theta_i = degrees_to_radians(theta_i_degrees);
	double epsilon, theta, xc, yc;
	double dtheta;
	double sb_avg, next_sb_avg, prev_sb_avg, grad_xistep, sb_grad, prev_sbgrad, rms_sbgrad_rel, prev_rms_sbgrad_rel, max_amp;
	bool abort_isofit = false;
	epsilon = 1 - qi;
	theta = theta_i;
	xc = xc_i;
	yc = yc_i;

	int lowest_harmonic, n_ellipse_amps, xc_ampnum, yc_ampnum, epsilon_ampnum, theta_ampnum;
	if (!fix_center) {
		lowest_harmonic = 1;
		xc_ampnum = 0;
		yc_ampnum = 1;
		epsilon_ampnum = 2;
		theta_ampnum = 3;
	} else {
		lowest_harmonic = 2;
		xc_ampnum = -1;
		yc_ampnum = -1;
		epsilon_ampnum = 0;
		theta_ampnum = 1;
	}
	n_ellipse_amps = 2*(3-lowest_harmonic);

	//const int n_higher_harmonics = 4; //  should be at least 2. Seems to be unstable if n_higher_harmonics > 4; probably matrices becoming singular for some xi vals
	int nmax_amp = n_ellipse_amps + 2*n_higher_harmonics;
	int n_harmonics_it; // will be reduced if singular matrix occurs
	int nmax_amp_it; // will be reduced if singular matrix occurs
	double *sb_residual = new double[npts_max];
	double *sb_weights = new double[npts_max];
	double *sbvals = new double[npts_max];
	double *sbvals_next = new double[npts_max];
	double *sbvals_prev = new double[npts_max];
	double *sbgrad_weights_next = new double[npts_max];
	double *sbgrad_weights_prev = new double[npts_max];
	double *Dvec = new double[nmax_amp];
	double *amp = new double[nmax_amp];
	double *amp_minres = new double[nmax_amp];
	double *amperrs = new double[nmax_amp];
	double **smatrix = new double*[nmax_amp];
	double **Smatrix = new double*[nmax_amp];
	for (i=0; i < nmax_amp; i++) {
		smatrix[i] = new double[npts_max];
		Smatrix[i] = new double[nmax_amp];
	}

	int sampling_mode; // Sampling modes: 0 = interpolation; 1 = pixel_integration; 2 = either 0/1 based on how big xi is; 3 = use sbprofile
	double sampling_noise;
	int it, jmax;
	int npts, npts_sample, next_npts, prev_npts, ngrad;
	double minchisq;
	bool already_switched = false;
	bool failed_isophote_fit;
	bool using_prev_sbgrad;
	bool do_parameter_search;
	bool tried_parameter_search;
	double rms_resid, rms_resid_min;
	double xc_minres, yc_minres, epsilon_minres, theta_minres, sbgrad_minres, rms_sbgrad_rel_minres, maxamp_minres;
	double maxamp_min;
	int it_minres;
	int xi_it, xi_i, xi_i_prev;
	int npts_minres, npts_sample_minres;
	if (pixel_noise==0) pixel_noise = 0.001; // just a hack for cases where there is no noise
	for (xi_it=0; xi_it < n_xivals; xi_it++) {
		xi_i = xi_ivals[xi_it];
		if (xi_it==0) xi_i_prev = xi_i;
		else xi_i_prev = xi_ivals[xi_it-1];
		xi = xivals[xi_i];
		grad_xistep = xi*xistep/2;
		if (grad_xistep < pixel_size) {
			grad_xistep = pixel_size;
		}
		if (verbose) cout << "xi_it=" << xi_it << " xi=" << xi << " epsilon=" << epsilon << " theta=" << radians_to_degrees(theta) << " xc=" << xc << " yc=" << yc << endl;
		if (xi_it==i_switch) {
			epsilon = epsilon0;
			theta = theta0;
			xc = xc0;
			yc = yc0;
			default_sampling_mode = default_sampling_mode_in;
			//cout << "sampling mode now: " << default_sampling_mode << endl;
		}
		it = 0;
		minchisq = 1e30;
		rms_resid_min = 1e30;
		maxamp_min = 1e30;
		double hterm;
		epsilon_prev = epsilon;
		theta_prev = theta;
		xc_prev = xc;
		yc_prev = yc;

		sampling_mode = default_sampling_mode;
		n_harmonics_it = n_higher_harmonics;
		do_parameter_search = false;
		nmax_amp_it = nmax_amp;
		//if (xi > 1.0) { // this is just a hack because it's having trouble with higher harmonics above m=4 beyond 1 arcsec or so
			//n_harmonics_it = 2;
			//nmax_amp_it = n_ellipse_amps + 2*n_harmonics_it;
		//}
		if (failed_isophote_fit) do_parameter_search = true; // start out with a parameter search
		tried_parameter_search = false;
		failed_isophote_fit = false;
		while (true) {
			using_prev_sbgrad = false;
			//cout << "Sampling mode: " << sampling_mode << endl;
			npts_sample = -1; // so it will choose automatically
			//cout << "EPSILON=" << epsilon << " THETA=" << theta << endl;
			sb_avg = sample_ellipse(verbose,xi,xistep,epsilon,theta,xc,yc,npts,npts_sample,emode,sampling_mode,sbvals,NULL,sbprofile,true,sb_residual,sb_weights,smatrix,lowest_harmonic,2+n_harmonics_it,use_polar_higher_harmonics);
			if (npts < npts_sample*npts_frac) {
				// if there aren't enough points/sectors, then it's not worth doing a parameter search (it can even backfire and result in contour crossings etc.); just move on
				do_parameter_search = false;
				tried_parameter_search = true;
				epsilon = epsilon_prev;
				theta = theta_prev;
				xc = xc_prev;
				yc = yc_prev;
				failed_isophote_fit = true;
				double nfrac = npts/((double) npts_sample);
				warn("not enough points being sampled; moving on to next ellipse (npts_frac=%g,npts_frac_threshold=%g,sb_avg=%g)",nfrac,npts_frac,sb_avg);
				break;
			}
			//if (do_parameter_search) {
				//do_parameter_search = false;
				//tried_parameter_search = true;
			//}
			if (do_parameter_search) {
				double ep, th;
				double epmin = epsilon - 0.1;
				double epmax = epsilon + 0.1;
				if (epmin < 0) epmin = 0.01;
				if (epmax > 1) epmax = 0.9;
				double thmin = theta - M_PI/4;
				double thmax = theta + M_PI/4;
				int parameter_search_nn = 100;
				double epstep = (epmax-epmin)/(parameter_search_nn-1);
				double thstep = (thmax-thmin)/(parameter_search_nn-1);
				int ii,jj;
				double residmin = 1e30;

				for (ii=0, ep=epmin; ii < parameter_search_nn; ii++, ep += epstep) {
					for (jj=0, th=thmin; jj < parameter_search_nn; jj++, th += thstep) {
						sample_ellipse(false,xi,xistep,ep,th,xc,yc,npts,npts_sample,emode,sampling_mode,sbvals,NULL,sbprofile,true,sb_residual,sb_weights,smatrix,lowest_harmonic,2+n_harmonics_it,use_polar_higher_harmonics);

						// now generate Dvec and Smatrix (s_transpose * s), then do inversion to get amplitudes A.
						fill_matrices(npts,nmax_amp_it,sb_residual,sb_weights,smatrix,Dvec,Smatrix,1.0);
						bool chol_status;
						chol_status = Cholesky_dcmp(Smatrix,nmax_amp_it);
						if (!chol_status) {
							//warn("amplitude matrix is not positive-definite");
							continue;
						}
						Cholesky_solve(Smatrix,Dvec,amp,nmax_amp_it);

						rms_resid = 0;
						for (i=0; i < npts; i++) {
							hterm = sb_residual[i];
							for (j=n_ellipse_amps; j < nmax_amp_it; j++) hterm -= amp[j]*smatrix[j][i];
							rms_resid += hterm*hterm;
						}
						rms_resid = sqrt(rms_resid/npts);

						if (rms_resid < residmin) {
							epsilon = ep;
							theta = th;
							residmin = rms_resid;
						}
					}
				}
				if (residmin==1e30) {
					warn("Cholesky decomposition failed during parameter search; repeating previous isophote parameters");
					failed_isophote_fit = true;
					break;
				}
				cout << "Smallest residuals for epsilon=" << epsilon << ", theta=" << theta << " during parameter search (rms_resid=" << residmin << ")" << endl;
				//sb_avg = sample_ellipse(verbose,xi,xistep,epsilon,theta,xc,yc,npts,npts_sample,emode,sampling_mode,sbvals,NULL,sbprofile,true,sb_residual,sb_weights,smatrix,lowest_harmonic,2+n_harmonics_it,use_polar_higher_harmonics);
				do_parameter_search = false;
				tried_parameter_search = true;
				it = 0; // it's effectively a do-over now
				minchisq = 1e30;
				rms_resid_min = 1e30;
				maxamp_min = 1e30;
				epsilon_prev = epsilon;
				theta_prev = theta;
				xc_prev = xc;
				yc_prev = yc;

				sampling_mode = default_sampling_mode;
				n_harmonics_it = n_higher_harmonics;
				nmax_amp_it = nmax_amp;
				failed_isophote_fit = false;
				continue;
			}

			if (verbose) {
				if (sb_avg*0.0 != 0.0) {
					for (i=0; i < npts; i++) {
						cout << sbvals[i] << " " << sb_residual[i] << endl;
					}
				}
			}

			if ((verbose) and (it==0)) {
				if (sampling_mode==0) cout << "Sampling mode: interpolation" << endl;
				else if (sampling_mode==1) cout << "Sampling mode: sector integration" << endl;
				else if (sampling_mode==3) cout << "Sampling mode: SB profile" << endl;
			}
			if (npts==0) {
				warn("isophote fit failed; no sampling points were accepted on ellipse");
				epsilon = epsilon_prev;
				theta = theta_prev;
				xc = xc_prev;
				yc = yc_prev;
				failed_isophote_fit = true;
				break;
			}
			// now generate Dvec and Smatrix (s_transpose * s), then do inversion to get amplitudes A. pixel errors are ignored here because they just cancel anyway
			fill_matrices(npts,nmax_amp_it,sb_residual,sb_weights,smatrix,Dvec,Smatrix,1.0);
			bool chol_status;
			chol_status = Cholesky_dcmp(Smatrix,nmax_amp_it);
			if (!chol_status) {
				if (n_harmonics_it > 2) {
					n_harmonics_it--;
					nmax_amp_it -= 2;
					continue;
				} else {
					warn("Cholesky decomposition failed, even with only two higher harmonics; repeating previous isophote parameters");
					epsilon = epsilon_prev;
					theta = theta_prev;
					xc = xc_prev;
					yc = yc_prev;
					failed_isophote_fit = true;
					break;
				}
			}
			Cholesky_solve(Smatrix,Dvec,amp,nmax_amp_it);

			rms_resid = 0;
			for (i=0; i < npts; i++) {
				hterm = sb_residual[i];
				for (j=n_ellipse_amps; j < nmax_amp_it; j++) hterm -= amp[j]*smatrix[j][i];
				rms_resid += hterm*hterm;
			}
			rms_resid = sqrt(rms_resid/npts);

			max_amp = -1e30;
			for (j=0; j < n_ellipse_amps; j++) if (abs(amp[j]) > max_amp) { max_amp = abs(amp[j]); jmax = j; }
			if (max_amp < maxamp_min) maxamp_min = max_amp;

			prev_sb_avg = sample_ellipse(verbose,xi-grad_xistep,xistep,epsilon,theta,xc,yc,prev_npts,npts_sample,emode,sampling_mode,sbvals_prev,sbgrad_weights_prev,sbprofile);
			next_sb_avg = sample_ellipse(verbose,xi+grad_xistep,xistep,epsilon,theta,xc,yc,next_npts,npts_sample,emode,sampling_mode,sbvals_next,sbgrad_weights_next,sbprofile);
			double nextstep = grad_xistep, prevstep = grad_xistep, gradstep;

			// Now we will see if not enough points are being sampled for the gradient, and will expand the stepsize to see if it helps (this can occur around masks)
			bool not_enough_pts = false;
			if (prev_npts < npts_sample*npts_frac) {
				warn("RUHROH! not enough points when getting gradient (npts_prev). Will increase stepsize (npts_sample=%i,nprev=%i,nnext=%i,npts=%i)",npts_sample,prev_npts,next_npts,npts);
				prev_sb_avg = sample_ellipse(verbose,xi-2*grad_xistep,xistep,epsilon,theta,xc,yc,prev_npts,npts_sample,emode,sampling_mode,sbvals_prev,sbgrad_weights_prev,sbprofile);
				if (prev_npts < npts_sample*npts_frac) {
					warn("RUHROH! not enough points when getting gradient (npts_next), even after reducing stepsize (npts_sample=%i,nprev=%i,nnext=%i,npts=%i)",npts_sample,prev_npts,next_npts,npts);
					not_enough_pts = true;
					//sb_grad = prev_sbgrad; // hack when all else fails
					//rms_sbgrad_rel = prev_rms_sbgrad_rel;
					//using_prev_sbgrad = true;
				}
				nextstep *= 2;
			}
			else if (next_npts < npts_sample*npts_frac) {
				warn("RUHROH! not enough points when getting gradient (npts_next) Will increase stepsize(npts_sample=%i,nprev=%i,nnext=%i,npts=%i)",npts_sample,prev_npts,next_npts,npts);
				next_sb_avg = sample_ellipse(verbose,xi+2*grad_xistep,xistep,epsilon,theta,xc,yc,next_npts,npts_sample,emode,sampling_mode,sbvals_next,sbgrad_weights_next,sbprofile);
				if (next_npts < npts_sample*npts_frac) {
					warn("RUHROH! not enough points when getting gradient (npts_next), even after reducing stepsize (npts_sample=%i,nprev=%i,nnext=%i,npts=%i)",npts_sample,prev_npts,next_npts,npts);
					not_enough_pts = true;
					//sb_grad = prev_sbgrad; // hack when all else fails
					//rms_sbgrad_rel = prev_rms_sbgrad_rel;
					//using_prev_sbgrad = true;
				}
				prevstep *= 2;
			}
			if ((not_enough_pts) or (prev_npts==0) or (next_npts==0)) {
				warn("isophote fit failed; not enough sampling points were accepted on ellipse for determining sbgrad");
				epsilon = epsilon_prev;
				theta = theta_prev;
				xc = xc_prev;
				yc = yc_prev;
				failed_isophote_fit = true;
				break;
			}

			if (!using_prev_sbgrad) {
				gradstep = nextstep+prevstep;
				if (!find_sbgrad(npts_sample,sbvals_prev,sbvals_next,sbgrad_weights_prev,sbgrad_weights_next,gradstep,sb_grad,rms_sbgrad_rel,ngrad))
				{
					abort_isofit = true;
					break;
				} else {
					if (verbose) cout << "it=" << it << " sbavg=" << sb_avg << " rms=" << rms_resid << " sbgrad=" << sb_grad << " maxamp=" << max_amp << " eps=" << epsilon << ", theta=" << radians_to_degrees(theta) << ", xc=" << xc << ", yc=" << yc << " (npts=" << npts << ")" << endl;
					if (ngrad < npts_sample*npts_frac) {
						warn("RUHROH! not enough points when getting gradient (ngrad=%i,npts_sample=%i,nprev=%i,nnext=%i,npts=%i)",ngrad,npts_sample,prev_npts,next_npts,npts);
						epsilon = epsilon_prev;
						theta = theta_prev;
						xc = xc_prev;
						yc = yc_prev;
						failed_isophote_fit = true;
						break;
					}
					prev_sbgrad = sb_grad;
					prev_rms_sbgrad_rel = rms_sbgrad_rel;
				}
			}
			//if (prev_npts < npts_sample*npts_frac) die();
			//if (next_npts < npts_sample*npts_frac) die();
			//if (ngrad < npts_sample*npts_frac) die();

			if (rms_resid < rms_resid_min) {
				// save params in case this solution is better than the final one
				for (j=0; j < nmax_amp_it; j++) {
					amp_minres[j] = amp[j];
				}
				rms_resid_min = rms_resid;
				it_minres = it;
				xc_minres = xc;
				yc_minres = yc;
				epsilon_minres = epsilon;
				theta_minres = theta;
				sbgrad_minres = sb_grad;
				rms_sbgrad_rel_minres = rms_sbgrad_rel;
				npts_minres = npts;
				npts_sample_minres = npts_sample;
				maxamp_minres = max_amp;
			}

			if (it >= min_it) {
				if ((max_amp < sbfrac*rms_resid) or (it==max_it)) break; // Jedrzejewsi includes the harmonic corrections when calculating the residuals for this criterion. Is it really necessary?
			}

			if (jmax==xc_ampnum) {
				if (emode==0) xc -= amp[xc_ampnum]/sb_grad;
				else xc -= amp[xc_ampnum]/sqrt(1-epsilon)/sb_grad;
			} else if (jmax==yc_ampnum) {
				if (emode==0) yc -= amp[yc_ampnum]*(1-epsilon)/sb_grad;
				else yc -= amp[yc_ampnum]*sqrt(1-epsilon)/sb_grad;
			} else if (jmax==epsilon_ampnum) {
				epsilon += -2*amp[epsilon_ampnum]*(1-epsilon)/(xi*sb_grad);
			} else if (jmax==theta_ampnum) {
				dtheta = 2*amp[theta_ampnum]*(1-epsilon)/(xi*sb_grad*(SQR(1-epsilon)-1));
				theta += dtheta;
				//if (theta > 0) theta -= M_PI;
			}
			it++;
			if ((xc_ampnum >= 0) and ((xc < pixel_xcvals[0]) or (xc > pixel_xcvals[npixels_x-1]))) {
				warn("isofit failed; ellipse center went outside the image (xc=%g, xamp=%g). Moving on to next isophote (npts=%i, npts_sample=%i, npts/npts_sample=%g)",xc,amp[xc_ampnum],npts,npts_sample,(((double) npts)/npts_sample));
				epsilon = epsilon_prev;
				theta = theta_prev;
				xc = xc_prev;
				yc = yc_prev;
				failed_isophote_fit = true;
				break;
			}
			if ((yc_ampnum >= 0) and ((yc < pixel_ycvals[0]) or (yc > pixel_ycvals[npixels_y-1]))) {
				warn("isofit failed; ellipse center went outside the image (yc=%g, yamp=%g). Moving on to next isophote (npts/npts_sample=%g)",yc,amp[yc_ampnum],(((double) npts)/npts_sample));
				epsilon = epsilon_prev;
				theta = theta_prev;
				xc = xc_prev;
				yc = yc_prev;
				failed_isophote_fit = true;
				break;
			}
			if ((epsilon > 1.0) or (epsilon < 0.0)) {
				double frac = ((double) npts)/npts_sample;
				if ((!tried_parameter_search) and (npts > npts_sample*npts_frac)) {
					if (epsilon > 1.0) warn("isofit failed; ellipticity went above 1.0. Will now try a parameter search in epsilon, theta (npts=%i, npts_sample=%i, npts/npts_sample=%g)",npts,npts_sample,frac);
					else warn("isofit failed; ellipticity went below 0.0 (epsilon=%g). Will now try a parameter search in epsilon, theta (npts=%i, npts_sample=%i, npts/npts_sample=%g)",epsilon,npts,npts_sample,frac);

					epsilon = epsilon_prev;
					theta = theta_prev;
					xc = xc_prev;
					yc = yc_prev;
					do_parameter_search = true;
					continue;
				} else {
					if (epsilon > 1.0) warn("isofit failed; ellipticity went above 1.0. Moving on to next isophote (npts=%i, npts_sample=%i, npts/npts_sample=%g)",npts,npts_sample,frac);
					else warn("isofit failed; ellipticity went below 0.0 (epsilon=%g). Moving on to next isophote (npts=%i, npts_sample=%i, npts/npts_sample=%g)",epsilon,npts,npts_sample,frac);

					epsilon = epsilon_prev;
					theta = theta_prev;
					xc = xc_prev;
					yc = yc_prev;
					if (n_harmonics_it > 2) {
						n_harmonics_it--;
						nmax_amp_it -= 2;
						continue;
					}
					failed_isophote_fit = true;
					break;
				}
			}
			if ((jmax==theta_ampnum) and (abs(dtheta) > M_PI)) {
				if ((!tried_parameter_search) and (npts > npts_sample*npts_frac)) {
					warn("isofit failed; position angle jumped by more than 180 degrees(dtheta=%g, theta_amp=%g). Will now try a parameter search in epsilon, theta (npts/npts_sample=%g)",radians_to_degrees(dtheta),amp[theta_ampnum],(((double) npts)/npts_sample));
					epsilon = epsilon_prev;
					theta = theta_prev;
					xc = xc_prev;
					yc = yc_prev;
					do_parameter_search = true;
					continue;
				} else {
					epsilon = epsilon_prev;
					theta = theta_prev;
					xc = xc_prev;
					yc = yc_prev;
					if (n_harmonics_it > 2) {
						n_harmonics_it--;
						nmax_amp_it -= 2;
						continue;
					}
					warn("isofit failed; position angle jumped by more than 180 degrees(dtheta=%g, theta_amp=%g). Moving on to next isophote (npts/npts_sample=%g)",radians_to_degrees(dtheta),amp[theta_ampnum],(((double) npts)/npts_sample));
					failed_isophote_fit = true;
					break;
				}
			}

			if ((sampling_mode==0) and (default_sampling_mode_in != 0) and (rms_sbgrad_rel > rms_sbgrad_rel_transition)) {
				warn("rms_sbgrad_rel (%g) greater than threshold (%g); switching to sector integration mode",rms_sbgrad_rel,rms_sbgrad_rel_transition);
				default_sampling_mode = 1;
				sampling_mode = default_sampling_mode;
				it=0;
				minchisq = 1e30;
				rms_resid_min = 1e30;
				maxamp_min = 1e30;
				if (already_switched) die("SWITCHING AGAIN");
				else already_switched = true;
				continue;
			}
			if (rms_sbgrad_rel > rms_sbgrad_rel_threshold) {
				warn("rms_sbgrad_rel (%g) greater than threshold; retaining previous isofit fit parameters for xi=%g",rms_sbgrad_rel,xi);
				epsilon = epsilon_prev;
				theta = theta_prev;
				xc = xc_prev;
				yc = yc_prev;
				failed_isophote_fit = true;
				break;
			}
		} // End of while loop

		if ((!failed_isophote_fit) and ((rms_sbgrad_rel_minres > rms_sbgrad_rel_threshold) or (npts_minres < npts_frac*npts_sample_minres))) {
			//warn("rms_sbgrad_rel (%g) greater than threshold; retaining previous isofit fit parameters",rms_sbgrad_rel);
			epsilon = epsilon_prev;
			theta = theta_prev;
			xc = xc_prev;
			yc = yc_prev;
			if (xi_it==0) {
				if (rms_sbgrad_rel_minres > rms_sbgrad_rel_threshold) warn("isophote fit cannot have rms_sbgrad_rel > threshold (%g vs %g) on first iteration",rms_sbgrad_rel_minres,rms_sbgrad_rel_threshold);
				else warn("isophote fit cannot have npts/npts_sample < npts_frac (%g vs %g) on first iteration",(((double) npts_minres)/npts_sample_minres),npts_frac);
				int npts_plot;
				if (verbose) sample_ellipse(verbose,xi,xistep,epsilon,theta,xc,yc,npts_plot,npts_sample,emode,sampling_mode,NULL,NULL,sbprofile,false,NULL,NULL,NULL,lowest_harmonic,2,false,true,&ellout); // make plot
				abort_isofit = true;
				break;
			}
		}
		if ((failed_isophote_fit) and (xi_it==0)) {
			warn("cannot have failed isophote fit on first iteration; aborting isofit");
			abort_isofit = true;
		}

		if (abort_isofit) break;
		if (npts==0) continue; // don't even record previous isophote parameters because we can't even get an estimate for sb_avg or its uncertainty
		if ((failed_isophote_fit) or (rms_sbgrad_rel_minres > rms_sbgrad_rel_threshold) or (npts_minres < npts_frac*npts_sample_minres)) {
			//cout << "The ellipse parameters are epsilon=" << epsilon << ", theta=" << radians_to_degrees(theta) << ", xc=" << xc << ", yc=" << yc << endl;
			//cout << "USING VERY LARGE ERRORS IN STRUCTURAL PARAMS" << endl;
			repeat_params[xi_i] = true;
			isophote_data.sb_avg_vals[xi_i] = sb_avg;
			isophote_data.sb_errs[xi_i] = rms_resid_min/sqrt(npts); // standard error of the mean
			isophote_data.qvals[xi_i] = isophote_data.qvals[xi_i_prev];
			isophote_data.thetavals[xi_i] = isophote_data.thetavals[xi_i_prev];
			isophote_data.xcvals[xi_i] = isophote_data.xcvals[xi_i_prev];
			isophote_data.ycvals[xi_i] = isophote_data.ycvals[xi_i_prev];
			//if (failed_isophote_fit) {
				isophote_data.q_errs[xi_i] = 1e30;
				isophote_data.theta_errs[xi_i] = 1e30;
				isophote_data.xc_errs[xi_i] = 1e30;
				isophote_data.yc_errs[xi_i] = 1e30;
			//} else {
				//isophote_data.q_errs[xi_i] = isophote_data.q_errs[xi_i_prev];
				//isophote_data.theta_errs[xi_i] = isophote_data.theta_errs[xi_i_prev];
				//isophote_data.xc_errs[xi_i] = isophote_data.xc_errs[xi_i_prev];
				//isophote_data.yc_errs[xi_i] = isophote_data.yc_errs[xi_i_prev];
			//}
			isophote_data.A3vals[xi_i] = isophote_data.A3vals[xi_i_prev];
			isophote_data.B3vals[xi_i] = isophote_data.B3vals[xi_i_prev];
			isophote_data.A4vals[xi_i] = isophote_data.A4vals[xi_i_prev];
			isophote_data.B4vals[xi_i] = isophote_data.B4vals[xi_i_prev];
			//if (failed_isophote_fit) {
				isophote_data.A3_errs[xi_i] = 1e30;
				isophote_data.B3_errs[xi_i] = 1e30;
				isophote_data.A4_errs[xi_i] = 1e30;
				isophote_data.B4_errs[xi_i] = 1e30;
			//} else {
				//isophote_data.A3_errs[xi_i] = isophote_data.A3_errs[xi_i_prev];
				//isophote_data.B3_errs[xi_i] = isophote_data.B3_errs[xi_i_prev];
				//isophote_data.A4_errs[xi_i] = isophote_data.A4_errs[xi_i_prev];
				//isophote_data.B4_errs[xi_i] = isophote_data.B4_errs[xi_i_prev];
			//}
			if (n_higher_harmonics > 2) {
				if (n_harmonics_it > 2) {
					isophote_data.A5vals[xi_i] = isophote_data.A5vals[xi_i_prev];
					isophote_data.B5vals[xi_i] = isophote_data.B5vals[xi_i_prev];
					isophote_data.A5_errs[xi_i] = isophote_data.A5_errs[xi_i_prev];
					isophote_data.B5_errs[xi_i] = isophote_data.B5_errs[xi_i_prev];
				} else {
					isophote_data.A5vals[xi_i] = 0;
					isophote_data.B5vals[xi_i] = 0;
					isophote_data.A5_errs[xi_i] = 1e-6;
					isophote_data.B5_errs[xi_i] = 1e-6;
				}
				if (n_harmonics_it > 3) {
					isophote_data.A6vals[xi_i] = isophote_data.A6vals[xi_i_prev];
					isophote_data.B6vals[xi_i] = isophote_data.B6vals[xi_i_prev];
					isophote_data.A6_errs[xi_i] = isophote_data.A6_errs[xi_i_prev];
					isophote_data.B6_errs[xi_i] = isophote_data.B6_errs[xi_i_prev];
				} else {
					isophote_data.A6vals[xi_i] = 0;
					isophote_data.B6vals[xi_i] = 0;
					isophote_data.A6_errs[xi_i] = 1e-6;
					isophote_data.B6_errs[xi_i] = 1e-6;
				}
			}
			if (verbose) {
				if (rms_sbgrad_rel_minres > rms_sbgrad_rel_threshold) cout << "rms_sbgrad_rel > threshold (" << rms_sbgrad_rel_minres << " vs " << rms_sbgrad_rel_threshold << ") --> repeating ellipse parameters, epsilon=" << epsilon << ", theta=" << radians_to_degrees(theta) << ", xc=" << xc << ", yc=" << yc << endl;
				if (npts_minres < npts_frac*npts_sample_minres) cout << "npts/npts_sample < npts_frac (" << (((double) npts_minres)/npts_sample) << " vs " << npts_frac << ") --> repeating ellipse parameters, epsilon=" << epsilon << ", theta=" << radians_to_degrees(theta) << ", xc=" << xc << ", yc=" << yc << endl;
				int npts_plot;
				sample_ellipse(verbose,xi,xistep,epsilon,theta,xc,yc,npts_plot,npts_sample,emode,sampling_mode,NULL,NULL,sbprofile,false,NULL,NULL,NULL,lowest_harmonic,2,false,true,&ellout); // make plot
			}

		} else {
			if (rms_resid > rms_resid_min) {
				// in this case the final solution was NOT the one with the smallest rms residuals
				for (j=0; j < nmax_amp_it; j++) {
					amp[j] = amp_minres[j];
				}
				xc = xc_minres;
				yc = yc_minres;
				epsilon = epsilon_minres;
				theta = theta_minres;
				sb_grad = sbgrad_minres;
				rms_sbgrad_rel = rms_sbgrad_rel_minres;
				max_amp = maxamp_minres;
				npts = npts_minres;
				npts_sample = npts_sample_minres;

				prev_sbgrad = sb_grad;
				prev_rms_sbgrad_rel = rms_sbgrad_rel;
			}

			if (verbose) {
				cout << "DONE! The final ellipse parameters are epsilon=" << epsilon << ", theta=" << radians_to_degrees(theta) << ", xc=" << xc << ", yc=" << yc << endl;
				cout << "Performed " << it << " iterations; minimum rms_resid=" << rms_resid_min << " achieved during iteration " << it_minres << endl;
			}

			sb_grad = abs(sb_grad);

			//sampling_noise = (sampling_mode==1) ? 2*rms_resid_min : dmin(rms_resid_min,pixel_noise); // if using pixel integration, noise is reduced due to averaging
			if (sampling_mode==1) {
				sampling_noise = 2*rms_resid_min; // if using pixel integration, noise is reduced due to averaging
			} else {
				if (npts <= 30) sampling_noise = pixel_noise/sqrt(3); // if too few points, rms_resid_min is not well-determined so it shouldn't be used for uncertainties
				else sampling_noise = rms_resid_min;
			}
			//sampling_noise = (sampling_mode==1) ? 2*rms_resid_min : pixel_noise;

			sample_ellipse(verbose,xi,xistep,epsilon,theta,xc,yc,npts,npts_sample,emode,sampling_mode,sbvals,NULL,sbprofile,true,sb_residual,sb_weights,smatrix,lowest_harmonic,2+n_harmonics_it,use_polar_higher_harmonics); // sample again in case we switched back to previous parameters
			fill_matrices(npts,nmax_amp_it,NULL,sb_weights,smatrix,NULL,Smatrix,sampling_noise);
			if (!Cholesky_dcmp(Smatrix,nmax_amp_it)) {
				warn("unexpected failure of Cholesky decomposition; isofit failed");
				return false;
			} else {
				Cholesky_fac_inverse(Smatrix,nmax_amp_it); // Now the lower triangle of Smatrix gives L_inv

				for (i=0; i < nmax_amp_it; i++) {
					amperrs[i] = 0;
					for (k=0; k <= i; k++) {
						amperrs[i] += SQR(Smatrix[i][k]); // just getting the diagonal elements of S_inverse from L_inv*L_inv^T
					}
					amperrs[i] = sqrt(amperrs[i]);
				}
			}
			//if (verbose) {
				//cout << "Untransformed: A3=" << amp[0] << " B3=" << amp[1] << " A4=" << amp[2] << " B4=" << amp[3] << endl;
				//cout << "Untransformed: A3_err=" << amperrs[0] << " B3_err=" << amperrs[1] << " A4_err=" << amperrs[2] << " B4_err=" << amperrs[3] << endl;
			//}

			for (i=n_ellipse_amps; i < nmax_amp_it; i++) {
				amp[i] = -amp[i]/(sb_grad*xi); // this will relate it to the contour shape amplitudes (when perturbing the elliptical radius)
				amperrs[i] = abs(amperrs[i]/(sb_grad*xi));
			}
			if (emode==0) {
				if (xc_ampnum >= 0) xc_err = amperrs[xc_ampnum]*sqrt(sb_grad);
				if (yc_ampnum >= 0) yc_err = amperrs[yc_ampnum]*sqrt((1-epsilon)/sb_grad);
			} else {
				if (xc_ampnum >= 0) xc_err = amperrs[xc_ampnum]*sqrt(1.0/sqrt(1-epsilon)/sb_grad);
				if (yc_ampnum >= 0) yc_err = amperrs[yc_ampnum]*sqrt(sqrt(1-epsilon)/sb_grad);
			}
			epsilon_err = amperrs[epsilon_ampnum]*sqrt(2*(1-epsilon)/(xi*sb_grad));
			if (epsilon_err > 1e10) die("absurd epsilon error! xi=%g, amperr=%g, sb_grad=%g",xi,amperrs[epsilon_ampnum],sb_grad);
			theta_err = amperrs[theta_ampnum]*sqrt(2*(1-epsilon)/(xi*sb_grad*(1-SQR(1-epsilon))));
			if (theta_err > degrees_to_radians(200)) warn("absurd theta error; amperr=%g,sbgrad=%g,epsilon=%g",amperrs[3],sb_grad,epsilon);

			//cout << "AMPERRS: " << amperrs[0] << " " << amperrs[1] << " " << amperrs[2] << " " << amperrs[3] << endl;
			if (verbose) {
				cout << "epsilon_err=" << epsilon_err << ", theta_err=" << radians_to_degrees(theta_err) << ", xc_err=" << xc_err << ", yc_err=" << yc_err << endl;
				cout << "A3=" << amp[n_ellipse_amps] << " B3=" << amp[n_ellipse_amps+1] << " A4=" << amp[n_ellipse_amps+2] << " B4=" << amp[n_ellipse_amps+3] << endl;
				cout << "A3_err=" << amperrs[n_ellipse_amps] << " B3_err=" << amperrs[n_ellipse_amps+1] << " A4_err=" << amperrs[n_ellipse_amps+2] << " B4_err=" << amperrs[n_ellipse_amps+3] << endl;
				if (n_harmonics_it > 2) {
					cout << "A5=" << amp[n_ellipse_amps+4] << " B5=" << amp[n_ellipse_amps+5] << endl;
					cout << "A5_err=" << amperrs[n_ellipse_amps+4] << " B5_err=" << amperrs[n_ellipse_amps+5] << endl;
				}
				if (n_harmonics_it > 3) {
					cout << "A6=" << amp[n_ellipse_amps+6] << " B6=" << amp[n_ellipse_amps+7] << endl;
					cout << "A6_err=" << amperrs[n_ellipse_amps+6] << " B6_err=" << amperrs[n_ellipse_amps+7] << endl;
				}

			}

			if ((verbose) and (sbprofile==NULL)) cout << "Best-fit rms_resid=" << rms_resid_min << ", sb_avg=" << sb_avg << ", sbgrad=" << sbgrad_minres << ", maxamp=" << maxamp_minres << ", rms_sbgrad_rel=" << rms_sbgrad_rel << endl;

			if (verbose) {
				int npts_plot;
				sample_ellipse(verbose,xi,xistep,epsilon,theta,xc,yc,npts_plot,npts_sample,emode,sampling_mode,NULL,NULL,sbprofile,false,NULL,NULL,NULL,lowest_harmonic,2,false,true,&ellout); // make plot
			}
			if (xi_it==0) {
				// save the initial best-fit values, since we'll use them again as our initial guess when we switch to stepping to smaller xi
				epsilon0 = epsilon;
				theta0 = theta;
				xc0 = xc;
				yc0 = yc;
			}

			isophote_data.sb_avg_vals[xi_i] = sb_avg;
			if (npts_minres < npts_frac_zeroweight*npts_sample_minres) {
				// Here we blow up the error (giving the data point zero weight) if there weren't enough points to sample the surface brightness well enough
				isophote_data.sb_errs[xi_i] = 1e30;
			} else {
				isophote_data.sb_errs[xi_i] = rms_resid_min/sqrt(npts); // standard error of the mean
			}
			isophote_data.qvals[xi_i] = 1 - epsilon;
			isophote_data.thetavals[xi_i] = theta;
			isophote_data.xcvals[xi_i] = xc;
			isophote_data.ycvals[xi_i] = yc;
			isophote_data.q_errs[xi_i] = epsilon_err;
			isophote_data.theta_errs[xi_i] = theta_err;
			isophote_data.xc_errs[xi_i] = xc_err;
			isophote_data.yc_errs[xi_i] = yc_err;
			isophote_data.A3vals[xi_i] = amp[n_ellipse_amps];
			isophote_data.B3vals[xi_i] = amp[n_ellipse_amps+1];
			isophote_data.A4vals[xi_i] = amp[n_ellipse_amps+2];
			isophote_data.B4vals[xi_i] = amp[n_ellipse_amps+3];
			isophote_data.A3_errs[xi_i] = amperrs[n_ellipse_amps];
			isophote_data.B3_errs[xi_i] = amperrs[n_ellipse_amps+1];
			isophote_data.A4_errs[xi_i] = amperrs[n_ellipse_amps+2];
			isophote_data.B4_errs[xi_i] = amperrs[n_ellipse_amps+3];

			if (n_higher_harmonics > 2) {
				if (n_harmonics_it > 2) {
					isophote_data.A5vals[xi_i] = amp[n_ellipse_amps+4];
					isophote_data.B5vals[xi_i] = amp[n_ellipse_amps+5];
					isophote_data.A5_errs[xi_i] = amperrs[n_ellipse_amps+4];
					isophote_data.B5_errs[xi_i] = amperrs[n_ellipse_amps+5];
				} else {
					isophote_data.A5vals[xi_i] = 0;
					isophote_data.B5vals[xi_i] = 0;
					isophote_data.A5_errs[xi_i] = 1e-6;
					isophote_data.B5_errs[xi_i] = 1e-6;
				}
				if (n_harmonics_it > 3) {
					isophote_data.A6vals[xi_i] = amp[n_ellipse_amps+6];
					isophote_data.B6vals[xi_i] = amp[n_ellipse_amps+7];
					isophote_data.A6_errs[xi_i] = amperrs[n_ellipse_amps+6];
					isophote_data.B6_errs[xi_i] = amperrs[n_ellipse_amps+7];
				} else {
					isophote_data.A6vals[xi_i] = 0;
					isophote_data.B6vals[xi_i] = 0;
					isophote_data.A6_errs[xi_i] = 1e-6;
					isophote_data.B6_errs[xi_i] = 1e-6;
				}
			}

		}
		if (abort_isofit) break;

		if ((sbprofile != NULL) and (verbose)) {
			// Now compare to true values...are uncertainties making sense?
			double true_q, true_epsilon, true_theta, true_xc, true_yc, true_A4, true_rms_sbgrad_rel;
			true_xc = sbprofile->x_center;
			true_yc = sbprofile->y_center;
			if (sbprofile->ellipticity_gradient==true) {
				double eps;
				sbprofile->ellipticity_function(xi,eps,true_theta);
				true_q = sqrt(1-eps);
				true_epsilon = 1-true_q;
			} else {
				true_q = sbprofile->q;
				true_epsilon = 1 - true_q;
				true_theta = sbprofile->theta;
			}
			true_A4 = 0.0;
			if (sbprofile->n_fourier_modes > 0) {
				for (int i=0; i < sbprofile->n_fourier_modes; i++) {
					if (sbprofile->fourier_mode_mvals[i]==n_ellipse_amps) true_A4 = sbprofile->fourier_mode_cosamp[i];
				}
			}
			cout << "TRUE MODEL: epsilon_true=" << true_epsilon << ", theta_true=" << radians_to_degrees(true_theta) << ", xc_true=" << true_xc << ", yc_true=" << true_yc << ", A4_true=" << true_A4 << endl;
			if (abs(xc-true_xc) > 3*xc_err) warn("RUHROH! xc off by more than 3*xc_err");
			if (abs(yc-true_yc) > 3*yc_err) warn("RUHROH! yc off by more than 3*yc_err");
			if (abs(epsilon - true_epsilon) > 3*epsilon_err) warn("RUHROH! epsilon off by more than 3*epsilon_err");
			if (abs(theta - true_theta) > 3*theta_err) warn("RUHROH! theta off by more than 3*theta_err");
			if (abs(amp[n_ellipse_amps+1] - true_A4) > 3*amperrs[n_ellipse_amps+1]) warn("RUHROH! A4 off by more than 3*A4_err");

			if ((abs(xc-true_xc) < 0.1*xc_err) and (abs(yc-true_yc) < 0.1*yc_err) and (abs(epsilon - true_epsilon) < 0.1*epsilon_err) and (abs(theta - true_theta) < 0.1*theta_err)) warn("Hmm, parameter residuals are all less than 0.1 times the uncertainties. Perhaps uncertainties are inflated?");

			//sb_avg = sample_ellipse(verbose,xi,xistep,true_epsilon,true_theta,true_xc,true_yc,npts,npts_sample,emode,sampling_mode,sbvals,sbprofile,true,sb_residual,smatrix,1,2+n_higher_harmonics,use_polar_higher_harmonics);
			//rms_resid = 0;
			//for (i=0; i < npts; i++) rms_resid += SQR(sb_residual[i]);
			//rms_resid = sqrt(rms_resid/npts);
			//fill_matrices(npts,nmax_amp,sb_residual,smatrix,Dvec,Smatrix,1.0);
			//if (!Cholesky_dcmp(Smatrix,nmax_amp)) die("Cholesky decomposition failed");
			//Cholesky_solve(Smatrix,Dvec,amp,nmax_amp);
			//double true_max_amp = -1e30;
			////if (verbose) cout << "AMPS: " << amp[0] << " " << amp[1] << " " << amp[2] << " " << amp[3] << endl;
			//for (j=0; j < 4; j++) if (abs(amp[j]) > true_max_amp) { true_max_amp = abs(amp[j]); }

			prev_sb_avg = sample_ellipse(verbose,xi-grad_xistep,xistep,true_epsilon,true_theta,true_xc,true_yc,prev_npts,npts_sample,emode,sampling_mode,sbvals_prev,sbgrad_weights_prev,sbprofile);
			next_sb_avg = sample_ellipse(verbose,xi+grad_xistep,xistep,true_epsilon,true_theta,true_xc,true_yc,next_npts,npts_sample,emode,sampling_mode,sbvals_next,sbgrad_weights_next,sbprofile);
			if ((prev_npts==0) or (next_npts==0)) {
				warn("isophote fit failed for true model; no sampling points were accepted on ellipse for determining sbgrad"); 
				abort_isofit = true;
				break;
			}
			else {
				if (!find_sbgrad(npts_sample,sbvals_prev,sbvals_next,sbgrad_weights_prev,sbgrad_weights_next,grad_xistep,sb_grad,true_rms_sbgrad_rel,ngrad)) { abort_isofit = true; break; }
			}

			// Now see what the rms_residual and amps are for true solution
			int true_npts;
			double true_sb_avg = sample_ellipse(verbose,xi,xistep,true_epsilon,true_theta,true_xc,true_yc,true_npts,npts_sample,emode,sampling_mode,NULL,NULL,sbprofile,true,sb_residual,sb_weights,smatrix,lowest_harmonic,2+n_higher_harmonics,use_polar_higher_harmonics);
			fill_matrices(npts,nmax_amp,sb_residual,sb_weights,smatrix,Dvec,Smatrix,1.0);
			if (!Cholesky_dcmp(Smatrix,nmax_amp)) die("Cholesky decomposition failed");
			Cholesky_solve(Smatrix,Dvec,amp,nmax_amp);

			double true_max_amp = -1e30;
			for (j=0; j < n_ellipse_amps; j++) if (abs(amp[j]) > true_max_amp) { true_max_amp = abs(amp[j]); }

			double rms_resid_true=0;
			for (i=0; i < npts; i++) {
				hterm = sb_residual[i];
				for (j=n_ellipse_amps; j < nmax_amp; j++) hterm -= amp[j]*smatrix[j][i];
				rms_resid_true += hterm*hterm;

			}
			rms_resid_true = sqrt(rms_resid_true/npts);

			for (i=n_ellipse_amps; i < nmax_amp; i++) amp[i] /= (sb_grad*xi); // this will relate it to the contour shape amplitudes (when perturbing the elliptical radius)

			cout << "Best-fit rms_resid=" << rms_resid_min << ", sb_avg=" << sb_avg << ", sbgrad=" << sbgrad_minres << ", maxamp=" << maxamp_minres << ", rms_sbgrad_rel=" << rms_sbgrad_rel << ", npts=" << npts << endl;
			cout << "True rms_resid=" << rms_resid_true << ", sb_avg=" << true_sb_avg << ", sbgrad=" << sb_grad << ", maxamp=" << true_max_amp << ", rms_sbgrad_rel=" << true_rms_sbgrad_rel << ", npts=" << true_npts << endl;
			cout << "True solution: A3=" << amp[n_ellipse_amps] << " B3=" << amp[n_ellipse_amps+1] << " A4=" << amp[n_ellipse_amps+2] << " B4=" << amp[n_ellipse_amps+3] << endl;
			double sbderiv = (sbprofile->sb_rsq(SQR(xi + grad_xistep)) - sbprofile->sb_rsq(SQR(xi-grad_xistep)))/(2*grad_xistep);

			cout << "sb from model at xi (no PSF or harmonics): " << sbprofile->sb_rsq(xi*xi) << ", sbgrad=" << sbderiv << endl;
			if ((rms_resid_true < rms_resid_min) and (rms_resid_min > 1e-8)) // we don't worry about this if rms_resid_min is super small
			{
				if (rms_sbgrad_rel < 0.5) warn("RUHROH! true solution had smaller rms_resid than best-fit, AND rms_sbgrad_rel < 0.5");
				else warn("true solution had smaller rms_resid than the best fit!");
			}
			if (rms_resid_true > rms_resid_min) cout << "NOTE: YOUR BEST-FIT SOLUTION HAS SMALLER RESIDUALS THAN TRUE SOLUTION" << endl;
		}
		if (verbose) cout << endl;
	}
	if (sbprofile != NULL) {
		int nn_plot = imax(100,6*n_xivals);
		sbprofile->plot_ellipticity_function(xi_min,xi_max,nn_plot);
	}

	/*
	double repeat_errfac = 1e30;
	for (i=0; i < n_xivals; i++) {
		if (repeat_params[i]) {
			isophote_data.q_errs[i] *= repeat_errfac;
			isophote_data.theta_errs[i] *= repeat_errfac;
			isophote_data.xc_errs[i] *= repeat_errfac;
			isophote_data.yc_errs[i] *= repeat_errfac;
			isophote_data.A3_errs[i] *= repeat_errfac;
			isophote_data.B3_errs[i] *= repeat_errfac;
			isophote_data.A4_errs[i] *= repeat_errfac;
			isophote_data.B4_errs[i] *= repeat_errfac;
		}
	}
	*/


	delete[] xivals;
	delete[] xi_ivals;
	delete[] xi_ivals_sorted;

	delete[] sb_residual;
	delete[] sb_weights;
	delete[] sbvals;
	delete[] sbvals_next;
	delete[] sbvals_prev;
	delete[] sbgrad_weights_next;
	delete[] sbgrad_weights_prev;
	delete[] Dvec;
	delete[] amp;
	delete[] amperrs;
	for (i=0; i < nmax_amp; i++) {
		delete[] smatrix[i];
		delete[] Smatrix[i];
	}
	delete[] smatrix;
	delete[] Smatrix;
	if (abort_isofit) return false;
	return true;
}

double ImagePixelData::sample_ellipse(const bool show_warnings, const double xi, const double xistep_in, const double epsilon, const double theta, const double xc, const double yc, int& npts, int& npts_sample, const int emode, int& sampling_mode, double *sbvals, double *sbgrad_wgts, SB_Profile* sbprofile, const bool fill_matrices, double* sb_residual, double *sb_weights, double** smatrix, const int ni, const int nf, const bool use_polar_higher_harmonics, const bool plot_ellipse, ofstream *ellout)
{
	if (epsilon > 1) die("epsilon cannot be greater than 1");
	//cout << xi << " " << epsilon << " " << theta << " " << xc << " " << yc << endl;
	int n_amps = 2*(nf-ni+1); // defaults: ni=1, nf=2
	double q, sqrtq, a0, costh, sinth;
	q = 1 - epsilon;
	sqrtq = sqrt(q);
	costh = cos(theta);
	sinth = sin(theta);
	static const int integration_pix_threshold = 60; // ARBITRARY! Should depend on S/N...figure this out later
	a0 = xi;
	if (emode > 0) a0 /= sqrtq; // semi-major axis

	int i, j, k, ii, jj;
	double x,y,xp,yp;
	double eta, eta_step, phi;
	double xstep = pixel_xcvals[1] - pixel_xcvals[0];
	double ystep = pixel_ycvals[1] - pixel_ycvals[0];
	double a0_npix = a0 / pixel_size;
	bool sector_warning = false;

	bool sector_integration;
	bool use_biweight_avg = false; // make this an option the user can change
	// NOTE: sampling_mode == 3 uses an sbprofile for testing purposes
	if ((sampling_mode==0) or (sampling_mode==3)) sector_integration = false;
	else if (sampling_mode==1) sector_integration = true;
	else {
		if (a0_npix < integration_pix_threshold) {
			sector_integration = false;
			sampling_mode = 0;
		}
		else {
			sector_integration = true;
			sampling_mode = 1;
		}
	}

	const int n_sectors = 36;
	double xistep = xistep_in;
	double xifac = (1+xistep);
	double xisqmax = SQR(xi*xifac);
	double xisqmin = SQR(xi/xifac);

	//double xisqmax = SQR(xi*(1 + xifac)/2);
	//double xisqmin = SQR(xi*(1 + 1.0/xifac)/2);

	double annulus_width = sqrt(xisqmax)-sqrt(xisqmin);
	//if (sbgrad_wgts == NULL) {
		// when getting the SB gradient, we allow a thicker annulus to reduce noise in the gradient; otherwise, reduce thickness if it's greater than 4 pixels across
			//while (annulus_width/pixel_size > 6.001) {
				//xistep *= 0.75;
				//xifac = (1+xistep);
				//xisqmax = SQR(xi*xifac);
				//xisqmin = SQR(xi/xifac);
				//annulus_width = sqrt(xisqmax)-sqrt(xisqmin);
			//}
	//}
	if ((sector_integration) and (sqrt(annulus_width*a0*q*M_2PI/n_sectors)/pixel_size < 2.25)) die("annulus sectors too small for sector integration"); // if the annulus is too thin, it causes problems

	const int max_npix_per_sector = 200;
	double *sb_sector;
	double **sector_sbvals;
	bool *sector_in_bounds;
	int *npixels_in_sector;

	if (!sector_integration) {
		if (npts_sample < 0) {
			if (emode==0) eta_step = pixel_size / xi;
			else eta_step = pixel_size / (xi/sqrtq);
			npts_sample = ((int) (M_2PI / eta_step)) + 1; // Then we will readjust eta_step to match npts_sample exactly
		}
	} else {
		if (npts_sample < 0) npts_sample = n_sectors;
		sb_sector = new double[npts_sample];
		sector_in_bounds = new bool[npts_sample];
		if (use_biweight_avg) sector_sbvals = new double*[npts_sample];
		npixels_in_sector = new int[npts_sample];
		for (i=0; i < npts_sample; i++) {
			sb_sector[i] = 0;
			sector_in_bounds[i] = true;
			npixels_in_sector[i] = 0;
			if (use_biweight_avg) sector_sbvals[i] = new double[max_npix_per_sector];
		}
	}
	eta_step = M_2PI / npts_sample;


	/*
	if ((plot_ellipse) and (sector_integration)) {
		ofstream annout("annulus.dat");
		int an_nn = 200;
		double xixi, etastep = M_2PI/(an_nn-1);

		xixi = sqrt(xisqmin);
		for (i=0, eta=0; i < an_nn; i++, eta += etastep) {
			if (emode==0) {
				xp = xixi*cos(eta);
				yp = xixi*q*sin(eta);
			} else {
				xp = xixi*cos(eta)/sqrtq;
				yp = xixi*sin(eta)*sqrtq;
			}
			x = xc + xp*costh - yp*sinth;
			y = yc + xp*sinth + yp*costh;
			annout << x << " " << y << " " << xp << " " << yp << endl;
		}
		annout << endl;

		xixi = sqrt(xisqmax);
		for (i=0, eta=0; i < an_nn; i++, eta += etastep) {
			if (emode==0) {
				xp = xixi*cos(eta);
				yp = xixi*q*sin(eta);
			} else {
				xp = xixi*cos(eta)/sqrtq;
				yp = xixi*sin(eta)*sqrtq;
			}
			x = xc + xp*costh - yp*sinth;
			y = yc + xp*sinth + yp*costh;
			annout << x << " " << y << " " << xp << " " << yp << endl;
		}
		annout << endl;
		double xi_step = (sqrt(xisqmax)-sqrt(xisqmin)) / (an_nn-1);
		for (i=0, eta=-eta_step/2; i < n_sectors; i++, eta += eta_step) {
			if (emode==0) {
				xp = cos(eta);
				yp = q*sin(eta);
			} else {
				xp = cos(eta)/sqrtq;
				yp = sin(eta)*sqrtq;
			}

			for (j=0, xixi = sqrt(xisqmin); j < an_nn; j++, xixi += xi_step) {
				x = xc + xixi*(xp*costh - yp*sinth);
				y = yc + xixi*(xp*sinth + yp*costh);
				annout << x << " " << y << " " << xixi*xp << " " << xixi*yp << endl;
			}
			annout << endl;
		}
		annout.close();
	}
	*/

	//ofstream secout;
	//if (plot_ellipse) secout.open("sectors.dat");
	//ofstream secout2("sectors2.dat");
	double sb, sb_avg = 0;
	double sbavg=0;
	double tt, uu, idoub, jdoub;
	npts = 0;
	bool out_of_bounds = false;
	int wtf=0;
	for (i=0, eta=0; i < npts_sample; i++, eta += eta_step) {
		if (sbvals != NULL) sbvals[i] = NAN; // if it doesn't get changed, then we know that point wasn't assign an SB
		if (emode==0) {
			xp = xi*cos(eta);
			yp = xi*q*sin(eta);
		} else {
			xp = xi*cos(eta)/sqrtq;
			yp = xi*sin(eta)*sqrtq;
		}

		x = xc + xp*costh - yp*sinth;
		y = yc + xp*sinth + yp*costh;
		if (plot_ellipse) (*ellout) << x << " " << y << " " << xp << " " << yp << endl;

		idoub = ((x - pixel_xcvals[0]) / xstep);
		jdoub = ((y - pixel_ycvals[0]) / ystep);
		if ((idoub < 0) or (idoub > (npixels_x-1))) {
			out_of_bounds = true;
			if (sector_integration) sector_in_bounds[i] = false;
			continue;
		}
		if ((jdoub < 0) or (jdoub > (npixels_y-1))) {
			out_of_bounds = true;
			if (sector_integration) sector_in_bounds[i] = false;
			continue;
		}
		ii = (int) idoub;
		jj = (int) jdoub;
		if ((!in_mask[ii][jj]) or (!in_mask[ii+1][jj]) or (!in_mask[ii][jj+1]) or (!in_mask[ii+1][jj+1])) {
			if (sector_integration) sector_in_bounds[i] = false;
			continue;
		}

		if (!sector_integration) {
			tt = (x - pixel_xcvals[ii]) / xstep;
			uu = (y - pixel_ycvals[jj]) / ystep;
			if ((tt < 0) or (tt > 1)) die("invalid interpolation parameters: tt=%g id=%g (ii=%i, npx=%i)",tt,idoub,ii,npixels_x);
			if ((uu < 0) or (uu > 1)) die("invalid interpolation parameters: uu=%g jd=%g (jj=%i, npy=%i)",uu,jdoub,jj,npixels_y);
			if ((sampling_mode==3) and (sbprofile != NULL)) {
				sb = sbprofile->surface_brightness(x,y);
			}
			else sb = (1-tt)*(1-uu)*surface_brightness[ii][jj] + tt*(1-uu)*surface_brightness[ii+1][jj] + (1-tt)*uu*surface_brightness[ii][jj+1] + tt*uu*surface_brightness[ii+1][jj+1];
			//if (plot_ellipse) secout << x << " " << y << endl;
			if ((show_warnings) and (sb*0.0 != 0.0)) {
				cout << "ii=" << ii << " jj=" << jj << endl;
				cout << "SB[ii][jj]=" << surface_brightness[ii][jj] << endl;
				cout << "SB[ii+1][jj]=" << surface_brightness[ii+1][jj] << endl;
				cout << "SB[ii][jj+1]=" << surface_brightness[ii][jj+1] << endl;
				cout << "SB[ii+1][jj+1]=" << surface_brightness[ii+1][jj+1] << endl;
				die("got surface brightness = NAN in data image");
			}

			sb_avg += sb;
			if (sbvals != NULL) sbvals[i] = sb;
			if (sbgrad_wgts != NULL) sbgrad_wgts[i] = SQR(pixel_noise)/4.0; // the same number of pixels is always used in interpolation mode, so weights should all be same
		}
		if (fill_matrices) {
			if (!sector_integration) {
				sb_residual[npts] = sb;
				sb_weights[npts] = 4.0/SQR(pixel_noise); // the same number of pixels is always used in interpolation mode, so weights should all be same
			}
			for (j=0, k=ni; j < n_amps; j += 2, k++) {
				if ((use_polar_higher_harmonics) and (k > 2)) {
					phi = atan(yp/xp);
					if (xp < 0) phi += M_PI;
					else if (yp < 0) phi += M_2PI;

					smatrix[j][npts] = cos(k*phi);
					//if (k==4) {
						//cout << (cos(k*phi)) << " " << amp << endl;
					//}
					smatrix[j+1][npts] = sin(k*phi);
				} else {
					smatrix[j][npts] = cos(k*eta);
					smatrix[j+1][npts] = sin(k*eta);
				}
			}
			if (k != nf+1) die("RUHROH");
		
		}
		npts++;
	}
	if (plot_ellipse) {
		// print initial point again just to close the curve
		if (emode==0) xp = xi;
		else xp = xi/sqrtq;
		yp = 0;

		x = xc + xp*costh - yp*sinth;
		y = yc + xp*sinth + yp*costh;
		(*ellout) << x << " " << y << " " << xp << " " << yp << endl << endl;
	}

	double avg_err=0;
	if ((sector_integration) and (npts > 0)) {
		double xmin, xmax, ymin, ymax;
		//xmin = xc - a0*(1+2*annulus_halfwidth_rel);
		//xmax = xc + a0*(1+2*annulus_halfwidth_rel);
		//ymin = yc - a0*(1+2*annulus_halfwidth_rel);
		//ymax = yc + a0*(1+2*annulus_halfwidth_rel);
		
		double amax = sqrt(xisqmax)/sqrt(q);
		xmin = xc - amax;
		xmax = xc + amax;
		ymin = yc - amax;
		ymax = yc + amax;
		int imin, imax, jmin, jmax;
		imin = (int) ((xmin - pixel_xcvals[0]) / xstep);
		jmin = (int) ((ymin - pixel_ycvals[0]) / ystep);
		imax = (int) ((xmax - pixel_xcvals[0]) / xstep);
		jmax = (int) ((ymax - pixel_ycvals[0]) / ystep);
		if (imax <= 0) die("something's gone horribly wrong");
		if (jmax <= 0) die("something's gone horribly wrong");
		if (imin < 0) imin = 0;
		if (jmin < 0) jmin = 0;
		if (imax >= npixels_x) imax = npixels_x-1;
		if (jmax >= npixels_y) jmax = npixels_y-1;
		double xisqval;
		double eta_i, eta_f, eta_width;
		eta_i = -eta_step/2; // since first sampling point is at y=0, the sector begins at negative eta
		eta_f = M_2PI-eta_step/2; // since first sampling point is at y=0, the sector begins at negative eta
		for (ii=imin; ii < imax; ii++) {
			for (jj=jmin; jj < jmax; jj++) {
				if (!in_mask[ii][jj]) continue;
				x = pixel_xcvals[ii] - xc;
				y = pixel_ycvals[jj] - yc;
				xp = x*costh + y*sinth;
				yp = -x*sinth + y*costh;
				if (emode==0) xisqval = xp*xp + SQR(yp/q);
				else xisqval = q*(xp*xp) + (yp*yp)/q;
				if ((xisqval > xisqmax) or (xisqval < xisqmin)) continue;
				eta = atan(yp/(q*xp));
				if (xp < 0) eta += M_PI;
				else if (yp < 0) eta += M_2PI;
				if (eta > eta_f) eta -= M_2PI;
				i = (int) ((eta - eta_i) / eta_step);
				if (sector_in_bounds[i]) {
					//cout << "In sector " << i << ": sb=" << surface_brightness[ii][jj] << " ij: " << ii << " " << jj << " comp to " << iivals[i] << " " << jjvals[i] << endl;
					if (use_biweight_avg) sector_sbvals[i][npixels_in_sector[i]] = surface_brightness[ii][jj];
					else sb_sector[i] += surface_brightness[ii][jj];
					//if (i==44) cout << "sector " << i << ": " << surface_brightness[ii][jj] << " pix=" << npixels_in_sector[i] << endl;
					//if (plot_ellipse) secout << pixel_xcvals[ii] << " " << pixel_ycvals[jj] << " " << xmin << " " << xmax << " " << ymin << " " << ymax << " WTF" << endl;
					//secout2 << ii << " " << jj << " " << imin << " " << imax << " " << jmin << " " << jmax << endl;
					npixels_in_sector[i]++;
				}
			}
		}

		int j;
		for (i=0, j=0; i < npts_sample; i++) {
			if (sector_in_bounds[i]) {
				if (npixels_in_sector[i]==0) {
					//cout << "a: " << a0 << " " << a0_npix << endl;
					if (sbvals != NULL) sbvals[i] = NAN;
					if (sbgrad_wgts != NULL) sbgrad_wgts[i] = NAN;
					npts--;
					sector_warning = true;
					//warn("zero pixels in sector? (i=%i)",i);
					if (fill_matrices) {
						for (ii=j; ii < npts; ii++) {
							for (k=0; k < n_amps; k+=2) {
								// this is really ugly!!!! have to move all remaining elements up because we've shortened the list of accepted points
								smatrix[k][ii] = smatrix[k][ii+1];
								smatrix[k+1][ii] = smatrix[k+1][ii+1];
							}
						}
					}
				} else {
					double pixerr = pixel_noise / sqrt(npixels_in_sector[i]);
					avg_err += pixerr;
					//cout << "pixerr=" << pixerr << endl;
					//	cout << "sector " << i << " error: " << pixerr << " " << npixels_in_sector[i] << " " << pixel_noise << endl;
					if (npixels_in_sector[i] < 5) {
						sector_warning = true;
						//warn("less than 5 pixels in sector (%i)",npixels_in_sector[i]);
					}
					if (!use_biweight_avg) {
						sb_sector[i] /= npixels_in_sector[i];
					} else {
						sort(npixels_in_sector[i],sector_sbvals[i]);

						int median_number = npixels_in_sector[i]/2;
						double median;
						if (npixels_in_sector[i] % 2 == 1) {
							median = sector_sbvals[i][median_number]; // odd number
						} else {
							median = 0.5*(sector_sbvals[i][median_number-1] + sector_sbvals[i][median_number]); // even number
						}

						double *distance_to_median = new double[npixels_in_sector[i]];
						for (k=0; k < npixels_in_sector[i]; k++) distance_to_median[k] = abs(sector_sbvals[i][k]-median);
						sort(npixels_in_sector[i],distance_to_median);

						double median_absolute_deviation;
						if (npixels_in_sector[i] % 2 == 1) median_absolute_deviation = distance_to_median[median_number];
						else median_absolute_deviation = 0.5*(distance_to_median[median_number] + distance_to_median[median_number+1]);

						double uu, uu_loc, biweight_location, location_numsum=0, location_denomsum=0;
						for (k=0; k < npixels_in_sector[i]; k++)
						{
							uu = (sector_sbvals[i][k] - median)/9.0/median_absolute_deviation;
							uu_loc = (sector_sbvals[i][k] - median)/6.0/median_absolute_deviation;
							if (abs(uu) < 1)
							{
								location_numsum += (sector_sbvals[i][k] - median)*SQR(1-uu_loc*uu_loc);
								location_denomsum += SQR(1-uu_loc*uu_loc);
							}
						}
						biweight_location = median + location_numsum/location_denomsum;
						delete[] distance_to_median;

						//sb_sector[i] = biweight_location;
						sb_sector[i] = median;
					}

					sb_avg += sb_sector[i];
					if (fill_matrices) {
						sb_residual[j] = sb_sector[i];
						sb_weights[j] = 1.0/SQR(pixerr);
						j++;
					}
					if (sbvals != NULL) sbvals[i] = sb_sector[i];
					if (sbgrad_wgts != NULL) sbgrad_wgts[i] = SQR(pixerr);
					//if (sbgrad_wgts != NULL) sbgrad_wgts[i] = 1.0;
				}
			}
		}
	}
	if (npts==0) sb_avg = 1e30;
	else {
		sb_avg /= npts;
		avg_err /= npts;
		if ((show_warnings) and (avg_err > (0.5*sb_avg))) warn("Average SB error is greater than 20\% of average isophote SB!! Try increasing annulus width (or fewer sectors) to reduce noise");
	}
	if (show_warnings) {
		if (out_of_bounds) warn("part of ellipse was out of bounds of the image");
		if (sector_warning) warn("less than 5 pixels in at least one sector");
	}

	if (fill_matrices) {
		for (i=0; i < npts; i++) sb_residual[i] -= sb_avg;
	}
	if (sector_integration) {
		delete[] sb_sector;
		delete[] sector_in_bounds;
		delete[] npixels_in_sector;
		if (use_biweight_avg) {
			for (i=0; i < npts_sample; i++) {
				delete[] sector_sbvals[i];
			}
			delete[] sector_sbvals;
		}
	}
	return sb_avg;
}

bool ImagePixelData::Cholesky_dcmp(double** a, int n)
{
	int i,j,k;

	a[0][0] = sqrt(a[0][0]);
	for (j=1; j < n; j++) a[j][0] /= a[0][0];

	bool status = true;
	for (i=1; i < n; i++) {
		//#pragma omp parallel for private(j,k) schedule(static)
		for (j=i; j < n; j++) {
			for (k=0; k < i; k++) {
				a[j][i] -= a[i][k]*a[j][k];
			}
		}
		if (a[i][i] < 0) {
			status = false;
		}
		a[i][i] = sqrt(abs(a[i][i]));
		for (j=i+1; j < n; j++) a[j][i] /= a[i][i];
	}
	return status;
}

void ImagePixelData::Cholesky_solve(double** a, double* b, double* x, int n)
{
	int i,k;
	double sum;
	for (i=0; i < n; i++) {
		for (sum=b[i], k=i-1; k >= 0; k--) sum -= a[i][k]*x[k];
		x[i] = sum / a[i][i];
	}
	for (i=n-1; i >= 0; i--) {
		for (sum=x[i], k=i+1; k < n; k++) sum -= a[k][i]*x[k];
		x[i] = sum / a[i][i];
	}	 
}

void ImagePixelData::Cholesky_fac_inverse(double** a, int n)
{
	int i,j,k;
	double sum;
	for (i=0; i < n; i++) {
		a[i][i] = 1.0/a[i][i];
		for (j=i+1; j < n; j++) {
			sum = 0.0;
			for (k=i; k < j; k++) sum -= a[j][k]*a[k][i];
			a[j][i] = sum / a[j][j];
		}
	}
}

/*
void ImagePixelData::add_point_image_from_centroid(ImageData* point_image_data, const double xmin, const double xmax, const double ymin, const double ymax, const double sb_threshold, const double pixel_error)
{
	int i,j;
	int imin=0, imax=npixels_x-1, jmin=0, jmax=npixels_y-1;
	bool passed_min=false;
	for (i=0; i < npixels_x; i++) {
		if ((passed_min==false) and ((xvals[i+1]+xvals[i]) > 2*xmin)) {
			imin = i;
			passed_min = true;
		} else if (passed_min==true) {
			if ((xvals[i+1]+xvals[i]) > 2*xmax) {
				imax = i-1;
				break;
			}
		}
	}
	passed_min = false;
	for (j=0; j < npixels_y; j++) {
		if ((passed_min==false) and ((yvals[j+1]+yvals[j]) > 2*ymin)) {
			jmin = j;
			passed_min = true;
		} else if (passed_min==true) {
			if ((yvals[j+1]+yvals[j]) > 2*ymax) {
				jmax = j-1;
				break;
			}
		}
	}
	if ((imin==imax) or (jmin==jmax)) die("window for centroid calculation has zero size");
	double centroid_x=0, centroid_y=0, centroid_err_x=0, centroid_err_y=0, total_flux=0;
	int np=0;
	double xm,ym;
	for (j=jmin; j <= jmax; j++) {
		for (i=imin; i <= imax; i++) {
			if (surface_brightness[i][j] > sb_threshold) {
				xm = 0.5*(xvals[i+1]+xvals[i]);
				ym = 0.5*(yvals[j+1]+yvals[j]);
				centroid_x += xm*surface_brightness[i][j];
				centroid_y += ym*surface_brightness[i][j];
				//centroid_err_x += xm*xm*surface_brightness[i][j];
				//centroid_err_y += ym*ym*surface_brightness[i][j];
				total_flux += surface_brightness[i][j];
				np++;
			}
		}
	}
	if (total_flux==0) die("Zero pixels are above the stated surface brightness threshold for calculating image centroid");
	//double avg_signal = total_flux / np;
	//cout << "Average signal: " << avg_signal << endl;
	centroid_x /= total_flux;
	centroid_y /= total_flux;
	double centroid_err_x_sb=0, centroid_err_y_sb=0;
	for (j=jmin; j <= jmax; j++) {
		for (i=imin; i <= imax; i++) {
			if (surface_brightness[i][j] > sb_threshold) {
				xm = 0.5*(xvals[i+1]+xvals[i]);
				ym = 0.5*(yvals[j+1]+yvals[j]);
				centroid_err_x_sb += SQR(xm-centroid_x);
				centroid_err_y_sb += SQR(ym-centroid_y);
			}
		}
	}

	// Finding an error based on the second moment seems flawed, since with enough pixels the centroid should be known very well.
	// For now, we choose the pixel size to give the error.
	//centroid_err_x /= total_flux;
	//centroid_err_y /= total_flux;
	//centroid_err_x = sqrt(centroid_err_x - SQR(centroid_x));
	//centroid_err_y = sqrt(centroid_err_y - SQR(centroid_y));
	centroid_err_x_sb = sqrt(centroid_err_x_sb)*pixel_error/total_flux;
	centroid_err_y_sb = sqrt(centroid_err_y_sb)*pixel_error/total_flux;
	centroid_err_x = xvals[1] - xvals[0];
	centroid_err_y = yvals[1] - yvals[0];
	centroid_err_x = sqrt(SQR(centroid_err_x) + SQR(centroid_err_x_sb));
	centroid_err_y = sqrt(SQR(centroid_err_y) + SQR(centroid_err_y_sb));
	//cout << "err_x_sb=" << centroid_err_x_sb << ", err_x=" << centroid_err_x << ", err_y_sb=" << centroid_err_y_sb << ", err_y=" << centroid_err_y << endl;
	double centroid_err = dmax(centroid_err_x,centroid_err_y);
	double flux_err = pixel_error / sqrt(np);
	//cout << "centroid = (" << centroid_x << "," << centroid_y << "), err=(" << centroid_err_x << "," << centroid_err_y << "), flux = " << total_flux << ", flux_err = " << flux_err << endl;
	lensvector pos; pos[0] = centroid_x; pos[1] = centroid_y;
	point_image_data->add_image(pos,centroid_err,total_flux,flux_err,0,0);
}
*/

void ImagePixelData::plot_surface_brightness(string outfile_root, bool show_only_mask, bool show_extended_mask, bool show_foreground_mask)
{
	string sb_filename = outfile_root + ".dat";
	string x_filename = outfile_root + ".x";
	string y_filename = outfile_root + ".y";

	ofstream pixel_image_file; lens->open_output_file(pixel_image_file,sb_filename);
	ofstream pixel_xvals; lens->open_output_file(pixel_xvals,x_filename);
	ofstream pixel_yvals; lens->open_output_file(pixel_yvals,y_filename);
	pixel_image_file << setiosflags(ios::scientific);
	int i,j;
	for (int i=0; i <= npixels_x; i++) {
		pixel_xvals << xvals[i] << endl;
	}
	for (int j=0; j <= npixels_y; j++) {
		pixel_yvals << yvals[j] << endl;
	}	
	if (show_extended_mask) {
		for (j=0; j < npixels_y; j++) {
			for (i=0; i < npixels_x; i++) {
				if ((!show_only_mask) or (extended_mask == NULL) or (extended_mask[i][j])) {
					pixel_image_file << surface_brightness[i][j];
				} else {
					pixel_image_file << "NaN";
				}
				if (i < npixels_x-1) pixel_image_file << " ";
			}
			pixel_image_file << endl;
		}
	} else if (show_foreground_mask) {
		for (j=0; j < npixels_y; j++) {
			for (i=0; i < npixels_x; i++) {
				if ((!show_only_mask) or (foreground_mask == NULL) or (foreground_mask[i][j])) {
					pixel_image_file << surface_brightness[i][j];
				} else {
					pixel_image_file << "NaN";
				}
				if (i < npixels_x-1) pixel_image_file << " ";
			}
			pixel_image_file << endl;
		}
	} else {
		for (j=0; j < npixels_y; j++) {
			for (i=0; i < npixels_x; i++) {
				if ((!show_only_mask) or (in_mask == NULL) or (in_mask[i][j])) {
					pixel_image_file << surface_brightness[i][j];
				} else {
					pixel_image_file << "NaN";
				}
				if (i < npixels_x-1) pixel_image_file << " ";
			}
			pixel_image_file << endl;
		}
	}
}

/***************************************** Functions in class ImagePixelGrid ****************************************/

ImagePixelGrid::ImagePixelGrid(QLens* lens_in, SourceFitMode mode, RayTracingMethod method, double xmin_in, double xmax_in, double ymin_in, double ymax_in, int x_N_in, int y_N_in, const bool raytrace) : lens(lens_in), xmin(xmin_in), xmax(xmax_in), ymin(ymin_in), ymax(ymax_in), x_N(x_N_in), y_N(y_N_in)
{
	source_fit_mode = mode;
	ray_tracing_method = method;
	setup_pixel_arrays();

	imggrid_zfactors = lens->reference_zfactors;
	imggrid_betafactors = lens->default_zsrc_beta_factors;

	pixel_xlength = (xmax-xmin)/x_N;
	pixel_ylength = (ymax-ymin)/y_N;
	pixel_area = pixel_xlength*pixel_ylength;
	triangle_area = 0.5*pixel_xlength*pixel_ylength;

	double x,y;
	int i,j;
	for (j=0; j <= y_N; j++) {
		y = ymin + j*pixel_ylength;
		for (i=0; i <= x_N; i++) {
			x = xmin + i*pixel_xlength;
			if ((i < x_N) and (j < y_N)) {
				center_pts[i][j][0] = x + 0.5*pixel_xlength;
				center_pts[i][j][1] = y + 0.5*pixel_ylength;
			}
			corner_pts[i][j][0] = x;
			corner_pts[i][j][1] = y;
		}
	}
	fit_to_data = NULL;
	if (raytrace) {
#ifdef USE_OPENMP
		double wtime0, wtime;
		if (lens->show_wtime) {
			wtime0 = omp_get_wtime();
		}
#endif
		setup_ray_tracing_arrays();
		if (lens->islens()) calculate_sourcepts_and_areas(true);
#ifdef USE_OPENMP
		if (lens->show_wtime) {
			wtime = omp_get_wtime() - wtime0;
			if (lens->mpi_id==0) cout << "Wall time for creating and ray-tracing image pixel grid: " << wtime << endl;
		}
#endif
	}
}

ImagePixelGrid::ImagePixelGrid(QLens* lens_in, SourceFitMode mode, RayTracingMethod method, double** sb_in, const int x_N_in, const int y_N_in, const int reduce_factor, double xmin_in, double xmax_in, double ymin_in, double ymax_in) : lens(lens_in), xmin(xmin_in), xmax(xmax_in), ymin(ymin_in), ymax(ymax_in)
{
	// I think this constructor is only used for the "reduce" option, which I never use anymore. Get rid of this option (and constructor) altogether?
	source_fit_mode = mode;
	ray_tracing_method = method;

	x_N = x_N_in / reduce_factor;
	y_N = y_N_in / reduce_factor;

	setup_pixel_arrays();

	imggrid_zfactors = lens->reference_zfactors;
	imggrid_betafactors = lens->default_zsrc_beta_factors;
	pixel_xlength = (xmax-xmin)/x_N;
	pixel_ylength = (ymax-ymin)/y_N;
	pixel_area = pixel_xlength*pixel_ylength;
	triangle_area = 0.5*pixel_xlength*pixel_ylength;

	int i,j;
	double x,y;
	int ii,jj;
	for (j=0; j <= y_N; j++) {
		y = ymin + j*pixel_ylength;
		for (i=0; i <= x_N; i++) {
			x = xmin + i*pixel_xlength;
			if ((i < x_N) and (j < y_N)) {
				center_pts[i][j][0] = x + 0.5*pixel_xlength;
				center_pts[i][j][1] = y + 0.5*pixel_ylength;
				//if (lens->islens()) lens->find_sourcept(center_pts[i][j],center_sourcepts[i][j],0,imggrid_zfactors,imggrid_betafactors);
			}
			corner_pts[i][j][0] = x;
			corner_pts[i][j][1] = y;
			//if (lens->islens()) lens->find_sourcept(corner_pts[i][j],corner_sourcepts[i][j],0,imggrid_zfactors,imggrid_betafactors);
			if ((i < x_N) and (j < y_N)) {
				surface_brightness[i][j] = 0;
				for (ii=0; ii < reduce_factor; ii++) {
					for (jj=0; jj < reduce_factor; jj++) {
						surface_brightness[i][j] += sb_in[i*reduce_factor+ii][j*reduce_factor+jj];
					}
				}
				surface_brightness[i][j] /= SQR(reduce_factor);
			}
		}
	}
	fit_to_data = NULL;
	setup_ray_tracing_arrays();
	if (lens->islens()) calculate_sourcepts_and_areas(true);
}

ImagePixelGrid::ImagePixelGrid(QLens* lens_in, SourceFitMode mode, RayTracingMethod method, ImagePixelData& pixel_data, const bool include_extended_mask, const bool ignore_mask, const bool verbal)
{
	// with this constructor, we create the arrays but don't actually make any lensing calculations, since these will be done during each likelihood evaluation
	lens = lens_in;
	source_fit_mode = mode;
	ray_tracing_method = method;
	pixel_data.get_grid_params(xmin,xmax,ymin,ymax,x_N,y_N);

	setup_pixel_arrays();

	imggrid_zfactors = lens->reference_zfactors;
	imggrid_betafactors = lens->default_zsrc_beta_factors;

	pixel_xlength = (xmax-xmin)/x_N;
	pixel_ylength = (ymax-ymin)/y_N;
	max_sb = -1e30;
	pixel_area = pixel_xlength*pixel_ylength;
	triangle_area = 0.5*pixel_xlength*pixel_ylength;

	int i,j;
	double x,y;
	for (j=0; j <= y_N; j++) {
		y = pixel_data.yvals[j];
		for (i=0; i <= x_N; i++) {
			x = pixel_data.xvals[i];
			if ((i < x_N) and (j < y_N)) {
				center_pts[i][j][0] = x + 0.5*pixel_xlength;
				center_pts[i][j][1] = y + 0.5*pixel_ylength;
				surface_brightness[i][j] = pixel_data.surface_brightness[i][j];
				if (!ignore_mask) {
					if (!include_extended_mask) fit_to_data[i][j] = pixel_data.in_mask[i][j];
					else fit_to_data[i][j] = pixel_data.extended_mask[i][j];
				}
				else fit_to_data[i][j] = true;
				if (surface_brightness[i][j] > max_sb) max_sb=surface_brightness[i][j];
			}
			corner_pts[i][j][0] = x;
			corner_pts[i][j][1] = y;
		}
	}
	setup_ray_tracing_arrays(verbal);
}

void ImagePixelGrid::setup_pixel_arrays()
{
	xy_N = x_N*y_N;
	if ((source_fit_mode==Cartesian_Source) or (source_fit_mode==Delaunay_Source)) n_active_pixels = 0;
	else n_active_pixels = xy_N;

	corner_pts = new lensvector*[x_N+1];
	corner_sourcepts = new lensvector*[x_N+1];
	center_pts = new lensvector*[x_N];
	center_sourcepts = new lensvector*[x_N];
	fit_to_data = new bool*[x_N];
	maps_to_source_pixel = new bool*[x_N];
	pixel_index = new int*[x_N];
	pixel_index_fgmask = new int*[x_N];
	mapped_cartesian_srcpixels = new vector<SourcePixelGrid*>*[x_N];
	mapped_delaunay_srcpixels = new vector<int>*[x_N];
	surface_brightness = new double*[x_N];
	foreground_surface_brightness = new double*[x_N];
	source_plane_triangle1_area = new double*[x_N];
	source_plane_triangle2_area = new double*[x_N];
	max_nsplit = imax(18,lens->default_imgpixel_nsplit);
	//max_nsplit = lens->default_imgpixel_nsplit;
	nsplits = new int*[x_N];
	subpixel_maps_to_srcpixel = new bool**[x_N];
	subpixel_center_pts = new lensvector**[x_N];
	subpixel_center_sourcepts = new lensvector**[x_N];
	twist_pts = new lensvector*[x_N];
	twist_status = new int*[x_N];

	int i,j,k;
	for (i=0; i <= x_N; i++) {
		corner_pts[i] = new lensvector[y_N+1];
		corner_sourcepts[i] = new lensvector[y_N+1];
	}
	for (i=0; i < x_N; i++) {
		center_pts[i] = new lensvector[y_N];
		center_sourcepts[i] = new lensvector[y_N];
		maps_to_source_pixel[i] = new bool[y_N];
		fit_to_data[i] = new bool[y_N];
		pixel_index[i] = new int[y_N];
		pixel_index_fgmask[i] = new int[y_N];
		surface_brightness[i] = new double[y_N];
		foreground_surface_brightness[i] = new double[y_N];
		source_plane_triangle1_area[i] = new double[y_N];
		source_plane_triangle2_area[i] = new double[y_N];
		mapped_cartesian_srcpixels[i] = new vector<SourcePixelGrid*>[y_N];
		mapped_delaunay_srcpixels[i] = new vector<int>[y_N];
		subpixel_maps_to_srcpixel[i] = new bool*[y_N];
		subpixel_center_pts[i] = new lensvector*[y_N];
		subpixel_center_sourcepts[i] = new lensvector*[y_N];
		nsplits[i] = new int[y_N];
		twist_pts[i] = new lensvector[y_N];
		twist_status[i] = new int[y_N];
		for (j=0; j < y_N; j++) {
			surface_brightness[i][j] = 0;
			foreground_surface_brightness[i][j] = 0;
			if ((source_fit_mode==Parameterized_Source) or (source_fit_mode==Shapelet_Source)) maps_to_source_pixel[i][j] = true; // in this mode you can always get a surface brightness for any image pixel
			else if (source_fit_mode==Delaunay_Source) maps_to_source_pixel[i][j] = true; // JUST A HACK FOR THE MOMENT--DELETE THIS ONCE THE IMAGE MAPPING FLAGS ARE WORKED OUT FOR THE DELAUNAY GRID
			//nsplits[i][j] = lens_in->default_imgpixel_nsplit; // default
			subpixel_maps_to_srcpixel[i][j] = new bool[max_nsplit*max_nsplit];
			subpixel_center_pts[i][j] = new lensvector[max_nsplit*max_nsplit];
			subpixel_center_sourcepts[i][j] = new lensvector[max_nsplit*max_nsplit];
			for (k=0; k < max_nsplit*max_nsplit; k++) subpixel_maps_to_srcpixel[i][j][k] = false;
		}
	}
	set_null_ray_tracing_arrays();
}

void ImagePixelGrid::set_null_ray_tracing_arrays()
{
	defx_corners = NULL;
	defy_corners = NULL;
	defx_centers = NULL;
	defy_centers = NULL;
	area_tri1 = NULL;
	area_tri2 = NULL;
	twistx = NULL;
	twisty = NULL;
	twiststat = NULL;

	masked_pixels_i = NULL;
	masked_pixels_j = NULL;
	emask_pixels_i = NULL;
	emask_pixels_j = NULL;
	masked_pixel_corner_i = NULL;
	masked_pixel_corner_j = NULL;
	masked_pixel_corner = NULL;
	masked_pixel_corner_up = NULL;
	extended_mask_subcell_i = NULL;
	extended_mask_subcell_j = NULL;
	extended_mask_subcell_index = NULL;
	defx_subpixel_centers = NULL;
	defy_subpixel_centers = NULL;
	ncvals = NULL;
}

void ImagePixelGrid::setup_ray_tracing_arrays(const bool verbal)
{
	int i,j,n,n_cell,n_corner;

	if ((!fit_to_data) or (lens->image_pixel_data == NULL)) {
		ntot_cells = x_N*y_N;
		ntot_cells_emask = x_N*y_N;
		ntot_corners = (x_N+1)*(y_N+1);
	} else {
		ntot_cells = 0;
		ntot_cells_emask = 0;
		ntot_corners = 0;
		for (i=0; i < x_N+1; i++) {
			for (j=0; j < y_N+1; j++) {
				if ((i < x_N) and (j < y_N) and (lens->image_pixel_data->in_mask[i][j])) ntot_cells++;
				if (((i < x_N) and (j < y_N) and (lens->image_pixel_data->in_mask[i][j])) or ((j < y_N) and (i > 0) and (lens->image_pixel_data->in_mask[i-1][j])) or ((i < x_N) and (j > 0) and (lens->image_pixel_data->in_mask[i][j-1])) or ((i > 0) and (j > 0) and (lens->image_pixel_data->in_mask[i-1][j-1]))) {
					ntot_corners++;
				}
			}
		}

		for (i=0; i < x_N+1; i++) {
			for (j=0; j < y_N+1; j++) {
				if ((i < x_N) and (j < y_N) and (lens->image_pixel_data->extended_mask[i][j])) ntot_cells_emask++;
			}
		}

	}

	if (defx_corners != NULL) delete_ray_tracing_arrays();
	// The following is used for the ray tracing
	defx_corners = new double[ntot_corners];
	defy_corners = new double[ntot_corners];
	defx_centers = new double[ntot_cells_emask];
	defy_centers = new double[ntot_cells_emask];
	area_tri1 = new double[ntot_cells];
	area_tri2 = new double[ntot_cells];
	twistx = new double[ntot_cells];
	twisty = new double[ntot_cells];
	twiststat = new int[ntot_cells];

	masked_pixels_i = new int[ntot_cells];
	masked_pixels_j = new int[ntot_cells];
	emask_pixels_i = new int[ntot_cells_emask];
	emask_pixels_j = new int[ntot_cells_emask];
	masked_pixel_corner_i = new int[ntot_corners];
	masked_pixel_corner_j = new int[ntot_corners];
	masked_pixel_corner = new int[ntot_cells];
	masked_pixel_corner_up = new int[ntot_cells];
	ncvals = new int*[x_N+1];
	for (i=0; i < x_N+1; i++) ncvals[i] = new int[y_N+1];

	
	n_cell=0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((!fit_to_data) or (lens->image_pixel_data == NULL) or (lens->image_pixel_data->in_mask[i][j])) {
				masked_pixels_i[n_cell] = i;
				masked_pixels_j[n_cell] = j;
				n_cell++;
			}
		}
	}

	n_cell=0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((!fit_to_data) or (lens->image_pixel_data == NULL) or (lens->image_pixel_data->extended_mask[i][j])) {
				emask_pixels_i[n_cell] = i;
				emask_pixels_j[n_cell] = j;
				n_cell++;
			}
		}
	}

	n_corner=0;
	if ((!fit_to_data) or (lens->image_pixel_data == NULL)) {
		for (j=0; j < y_N+1; j++) {
			for (i=0; i < x_N+1; i++) {
				ncvals[i][j] = -1;
				if (((i < x_N) and (j < y_N)) or ((j < y_N) and (i > 0)) or ((i < x_N) and (j > 0)) or ((i > 0) and (j > 0))) {
					masked_pixel_corner_i[n_corner] = i;
					masked_pixel_corner_j[n_corner] = j;
					ncvals[i][j] = n_corner;
					n_corner++;
				}
			}
		}
	} else {
		for (j=0; j < y_N+1; j++) {
			for (i=0; i < x_N+1; i++) {
				ncvals[i][j] = -1;
				if (((i < x_N) and (j < y_N) and (lens->image_pixel_data->in_mask[i][j])) or ((j < y_N) and (i > 0) and (lens->image_pixel_data->in_mask[i-1][j])) or ((i < x_N) and (j > 0) and (lens->image_pixel_data->in_mask[i][j-1])) or ((i > 0) and (j > 0) and (lens->image_pixel_data->in_mask[i-1][j-1]))) {
				//if (((i < x_N) and (j < y_N) and (lens->image_pixel_data->extended_mask[i][j])) or ((j < y_N) and (i > 0) and (lens->image_pixel_data->extended_mask[i-1][j])) or ((i < x_N) and (j > 0) and (lens->image_pixel_data->extended_mask[i][j-1])) or ((i > 0) and (j > 0) and (lens->image_pixel_data->extended_mask[i-1][j-1]))) {
					masked_pixel_corner_i[n_corner] = i;
					masked_pixel_corner_j[n_corner] = j;
					if (i > (x_N+1)) die("FUCK! corner i is huge from the get-go");
					if (j > (y_N+1)) die("FUCK! corner j is huge from the get-go");
					ncvals[i][j] = n_corner;
					n_corner++;
				}
			}
		}
	}
	//cout << "corner count: " << n_corner << " " << ntot_corners << endl;
	for (int n=0; n < ntot_cells; n++) {
		i = masked_pixels_i[n];
		j = masked_pixels_j[n];
		masked_pixel_corner[n] = ncvals[i][j];
		masked_pixel_corner_up[n] = ncvals[i][j+1];
	}
	for (int n=0; n < ntot_corners; n++) {
		i = masked_pixel_corner_i[n];
		j = masked_pixel_corner_j[n];
		if (i > (x_N+1)) die("FUCK! corner i is huge");
		if (j > (y_N+1)) die("FUCK! corner j is huge");
	}

	//double mask_min_r = 1e30;
	//if (lens->image_pixel_data) {
		//for (i=0; i < x_N; i++) {
			//for (j=0; j < y_N; j++) {
				//if (lens->image_pixel_data->in_mask[i][j]) {
					//double r = sqrt(SQR(center_pts[i][j][0]) + SQR(center_pts[i][j][1]));
					//if (r < mask_min_r) mask_min_r = r;
				//}
			//}
		//}
	//}
	//if (lens->mpi_id==0) cout << "HACK: mask_min_r=" << mask_min_r << endl;

	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++) {
			mapped_cartesian_srcpixels[i][j].clear();
			mapped_delaunay_srcpixels[i][j].clear();
		}
	}
	int nsplit = (lens->split_high_mag_imgpixels) ? 1 : lens->default_imgpixel_nsplit;
	int emask_nsplit = (lens->split_high_mag_imgpixels) ? 1 : lens->emask_imgpixel_nsplit;
	set_nsplits(lens->image_pixel_data,nsplit,emask_nsplit,lens->split_imgpixels);

	ntot_subpixels = 0;

	extended_mask_subcell_i = NULL;
	extended_mask_subcell_j = NULL;
	extended_mask_subcell_index = NULL;
	defx_subpixel_centers = NULL;
	defy_subpixel_centers = NULL;

	//if ((lens->split_imgpixels) and (!lens->split_high_mag_imgpixels)) { 
	if (lens->split_imgpixels) { 
		// if split_high_mag_imgpixels is on, this part will be deferred until after the ray-traced pixel areas have been
		// calculated (to get magnifications to use as criterion on whether to split or nit)
		setup_subpixel_ray_tracing_arrays(verbal);
	}
}

void ImagePixelGrid::setup_subpixel_ray_tracing_arrays(const bool verbal)
{
	ntot_subpixels = 0;
	int i,j;
	int nsplitpix = 0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((!fit_to_data) or (fit_to_data[i][j]) or (lens->image_pixel_data == NULL) or (lens->image_pixel_data->extended_mask[i][j])) {
				ntot_subpixels += INTSQR(nsplits[i][j]);
				if ((verbal) and (nsplits[i][j] > 1)) nsplitpix++;
			}
		}
	}
	if ((verbal) and (lens->mpi_id==0)) {
		cout << "Number of split image pixels: " << nsplitpix << endl;
		cout << "Total number of image pixels/subpixels: " << ntot_subpixels << endl;
	}

	if (extended_mask_subcell_i != NULL) delete[] extended_mask_subcell_i;
	if (extended_mask_subcell_j != NULL) delete[] extended_mask_subcell_j;
	if (extended_mask_subcell_index != NULL) delete[] extended_mask_subcell_index;

	extended_mask_subcell_i = new int[ntot_subpixels];
	extended_mask_subcell_j = new int[ntot_subpixels];
	extended_mask_subcell_index = new int[ntot_subpixels];

	int n_subpixel = 0;
	int k;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((!fit_to_data) or (fit_to_data[i][j]) or (lens->image_pixel_data == NULL) or (lens->image_pixel_data->extended_mask[i][j])) {
				for (k=0; k < INTSQR(nsplits[i][j]); k++) {
					extended_mask_subcell_i[n_subpixel] = i;
					extended_mask_subcell_j[n_subpixel] = j;
					extended_mask_subcell_index[n_subpixel] = k;
					n_subpixel++;
				}
			}
		}
	}

	if (defx_subpixel_centers != NULL) delete[] defx_subpixel_centers;
	if (defy_subpixel_centers != NULL) delete[] defy_subpixel_centers;

	defx_subpixel_centers = new double[ntot_subpixels];
	defy_subpixel_centers = new double[ntot_subpixels];
}

void ImagePixelGrid::delete_ray_tracing_arrays()
{
	if (defx_corners != NULL) delete[] defx_corners;
	if (defy_corners != NULL) delete[] defy_corners;
	if (defx_centers != NULL) delete[] defx_centers;
	if (defx_centers != NULL) delete[] defy_centers;
	if (area_tri1 != NULL) delete[] area_tri1;
	if (area_tri2 != NULL) delete[] area_tri2;
	if (twistx != NULL) delete[] twistx;
	if (twisty != NULL) delete[] twisty;
	if (twiststat != NULL) delete[] twiststat;

	if (masked_pixels_i != NULL) delete[] masked_pixels_i;
	if (masked_pixels_j != NULL) delete[] masked_pixels_j;
	if (emask_pixels_i != NULL) delete[] emask_pixels_i;
	if (emask_pixels_j != NULL) delete[] emask_pixels_j;
	if (masked_pixel_corner_i != NULL) delete[] masked_pixel_corner_i;
	if (masked_pixel_corner_j != NULL) delete[] masked_pixel_corner_j;
	if (masked_pixel_corner != NULL) delete[] masked_pixel_corner;
	if (masked_pixel_corner_up != NULL) delete[] masked_pixel_corner_up;
	if (lens->split_imgpixels) {
		if (extended_mask_subcell_i != NULL) delete[] extended_mask_subcell_i;
		if (extended_mask_subcell_j != NULL) delete[] extended_mask_subcell_j;
		if (extended_mask_subcell_index != NULL) delete[] extended_mask_subcell_index;
		if (defx_subpixel_centers != NULL) delete[] defx_subpixel_centers;
		if (defx_subpixel_centers != NULL) delete[] defy_subpixel_centers;
	}
	if (ncvals != NULL) {
		for (int i=0; i < x_N+1; i++) delete[] ncvals[i];
		delete[] ncvals;
	}
	set_null_ray_tracing_arrays();
}

inline bool ImagePixelGrid::test_if_between(const double& p, const double& a, const double& b)
{
	if ((b>a) and (p>a) and (p<b)) return true;
	else if ((a>b) and (p>b) and (p<a)) return true;
	return false;
}

void ImagePixelGrid::calculate_sourcepts_and_areas(const bool raytrace_pixel_centers, const bool verbal)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*(lens->group_comm), *(lens->mpi_group), &sub_comm);
#endif

	//long int ntot_cells_check = 0;
	//long int ntot_corners_check = 0;
	//for (i=0; i < x_N+1; i++) {
		//for (j=0; j < y_N+1; j++) {
			//if ((i < x_N) and (j < y_N) and (lens->image_pixel_data->in_mask[i][j])) ntot_cells_check++;
			//if (((i < x_N) and (j < y_N) and (lens->image_pixel_data->in_mask[i][j])) or ((j < y_N) and (i > 0) and (lens->image_pixel_data->in_mask[i-1][j])) or ((i < x_N) and (j > 0) and (lens->image_pixel_data->in_mask[i][j-1])) or ((i > 0) and (j > 0) and (lens->image_pixel_data->in_mask[i-1][j-1]))) {
			////if ((i < x_N) and (j < y_N) and (lens->image_pixel_data->extended_mask[i][j])) ntot_cells_check++;
			////if (((i < x_N) and (j < y_N) and (lens->image_pixel_data->extended_mask[i][j])) or ((j < y_N) and (i > 0) and (lens->image_pixel_data->extended_mask[i-1][j])) or ((i < x_N) and (j > 0) and (lens->image_pixel_data->extended_mask[i][j-1])) or ((i > 0) and (j > 0) and (lens->image_pixel_data->extended_mask[i-1][j-1]))) {
				//ntot_corners_check++;
			//}
		//}
	//}
	//if (ntot_cells_check != ntot_cells) die("ntot_cells does not equal the value assigned when image grid created");
	//if (ntot_corners_check != ntot_corners) die("ntot_corners does not equal the value assigned when image grid created");

	int i,j,k,n,n_cell,n_corner,n_yp;

	int mpi_chunk, mpi_start, mpi_end;
	mpi_chunk = ntot_corners / lens->group_np;
	mpi_start = lens->group_id*mpi_chunk;
	if (lens->group_id == lens->group_np-1) mpi_chunk += (ntot_corners % lens->group_np); // assign the remainder elements to the last mpi process
	mpi_end = mpi_start + mpi_chunk;

	int mpi_chunk2, mpi_start2, mpi_end2;
	mpi_chunk2 = ntot_cells / lens->group_np;
	mpi_start2 = lens->group_id*mpi_chunk2;
	if (lens->group_id == lens->group_np-1) mpi_chunk2 += (ntot_cells % lens->group_np); // assign the remainder elements to the last mpi process
	mpi_end2 = mpi_start2 + mpi_chunk2;

	int mpi_chunk4, mpi_start4, mpi_end4;
	mpi_chunk4 = ntot_cells_emask / lens->group_np;
	mpi_start4 = lens->group_id*mpi_chunk4;
	if (lens->group_id == lens->group_np-1) mpi_chunk4 += (ntot_cells_emask % lens->group_np); // assign the remainder elements to the last mpi process
	mpi_end4 = mpi_start4 + mpi_chunk4;

	#pragma omp parallel
	{
		int thread;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
		lensvector d1,d2,d3,d4;
		//int ii,jj;
		#pragma omp for private(n,i,j) schedule(dynamic)
		for (n=mpi_start; n < mpi_end; n++) {
			//j = n / (x_N+1);
			//i = n % (x_N+1);
			j = masked_pixel_corner_j[n];
			i = masked_pixel_corner_i[n];
			//cout << i << " " << j << " " << n << " " << ntot_corners << " " << mpi_end << endl;
			lens->find_sourcept(corner_pts[i][j],defx_corners[n],defy_corners[n],thread,imggrid_zfactors,imggrid_betafactors);
		}
#ifdef USE_MPI
		#pragma omp master
		{
			if (lens->group_np > 1) {
				int id, chunk, start;
				for (id=0; id < lens->group_np; id++) {
					chunk = ntot_corners / lens->group_np;
					start = id*chunk;
					if (id == lens->group_np-1) chunk += (ntot_corners % lens->group_np); // assign the remainder elements to the last mpi process
					MPI_Bcast(defx_corners+start,chunk,MPI_DOUBLE,id,sub_comm);
					MPI_Bcast(defy_corners+start,chunk,MPI_DOUBLE,id,sub_comm);
				}
			}
		}
		#pragma omp barrier
#endif
		#pragma omp for private(n_cell,i,j,n,n_yp) schedule(dynamic)
		for (n_cell=mpi_start2; n_cell < mpi_end2; n_cell++) {
			j = masked_pixels_j[n_cell];
			i = masked_pixels_i[n_cell];

			//n = j*(x_N+1)+i;
			//n_yp = (j+1)*(x_N+1)+i;
			n = masked_pixel_corner[n_cell];
			n_yp = masked_pixel_corner_up[n_cell];
			d1[0] = defx_corners[n] - defx_corners[n+1];
			d1[1] = defy_corners[n] - defy_corners[n+1];
			d2[0] = defx_corners[n_yp] - defx_corners[n];
			d2[1] = defy_corners[n_yp] - defy_corners[n];
			d3[0] = defx_corners[n_yp+1] - defx_corners[n_yp];
			d3[1] = defy_corners[n_yp+1] - defy_corners[n_yp];
			d4[0] = defx_corners[n+1] - defx_corners[n_yp+1];
			d4[1] = defy_corners[n+1] - defy_corners[n_yp+1];

			twiststat[n_cell] = 0;
			double xa,ya,xb,yb,xc,yc,xd,yd,slope1,slope2;
			xa=defx_corners[n];
			ya=defy_corners[n];
			xb=defx_corners[n_yp];
			yb=defy_corners[n_yp];
			xc=defx_corners[n_yp+1];
			yc=defy_corners[n_yp+1];
			xd=defx_corners[n+1];
			yd=defy_corners[n+1];
			slope1 = (yb-ya)/(xb-xa);
			slope2 = (yc-yd)/(xc-xd);
			twistx[n_cell] = (yd-ya+xa*slope1-xd*slope2)/(slope1-slope2);
			twisty[n_cell] = (twistx[n_cell]-xa)*slope1+ya;
			if ((test_if_between(twistx[n_cell],xa,xb)) and (test_if_between(twisty[n_cell],ya,yb)) and (test_if_between(twistx[n_cell],xc,xd)) and (test_if_between(twisty[n_cell],yc,yd))) {
				twiststat[n_cell] = 1;
				d2[0] = twistx[n_cell] - defx_corners[n];
				d2[1] = twisty[n_cell] - defy_corners[n];
				d4[0] = twistx[n_cell] - defx_corners[n_yp+1];
				d4[1] = twisty[n_cell] - defy_corners[n_yp+1];
			} else {
				slope1 = (yd-ya)/(xd-xa);
				slope2 = (yc-yb)/(xc-xb);
				twistx[n_cell] = (yb-ya+xa*slope1-xb*slope2)/(slope1-slope2);
				twisty[n_cell] = (twistx[n_cell]-xa)*slope1+ya;
				if ((test_if_between(twistx[n_cell],xa,xd)) and (test_if_between(twisty[n_cell],ya,yd)) and (test_if_between(twistx[n_cell],xb,xc)) and (test_if_between(twisty[n_cell],yb,yc))) {
					twiststat[n_cell] = 2;
					d1[0] = defx_corners[n] - twistx[n_cell];
					d1[1] = defy_corners[n] - twisty[n_cell];
					d3[0] = defx_corners[n_yp+1] - twistx[n_cell];
					d3[1] = defy_corners[n_yp+1] - twisty[n_cell];
				}
			}

			area_tri1[n_cell] = 0.5*abs(d1 ^ d2);
			area_tri2[n_cell] = 0.5*abs(d3 ^ d4);
		}

		if ((!lens->split_imgpixels) or (raytrace_pixel_centers)) {
			#pragma omp for private(n_cell,i,j,n,n_yp) schedule(dynamic)
			for (n_cell=mpi_start4; n_cell < mpi_end4; n_cell++) {
				j = emask_pixels_j[n_cell];
				i = emask_pixels_i[n_cell];
				lens->find_sourcept(center_pts[i][j],defx_centers[n_cell],defy_centers[n_cell],thread,imggrid_zfactors,imggrid_betafactors);
			}
		}
	}

#ifdef USE_MPI
	if (lens->group_np > 1) {
		int id, chunk, start;
		int id2, chunk2, start2;
		for (id=0; id < lens->group_np; id++) {
			chunk = ntot_cells / lens->group_np;
			start = id*chunk;
			if (id == lens->group_np-1) chunk += (ntot_cells % lens->group_np); // assign the remainder elements to the last mpi process
			if ((!lens->split_imgpixels) or (raytrace_pixel_centers)) {
				chunk2 = ntot_cells_emask / lens->group_np;
				start2 = id*chunk2;
				MPI_Bcast(defx_centers+start2,chunk2,MPI_DOUBLE,id,sub_comm);
				MPI_Bcast(defy_centers+start2,chunk2,MPI_DOUBLE,id,sub_comm);
			}
			MPI_Bcast(area_tri1+start,chunk,MPI_DOUBLE,id,sub_comm);
			MPI_Bcast(area_tri2+start,chunk,MPI_DOUBLE,id,sub_comm);
			MPI_Bcast(twistx+start,chunk,MPI_DOUBLE,id,sub_comm);
			MPI_Bcast(twisty+start,chunk,MPI_DOUBLE,id,sub_comm);
			MPI_Bcast(twiststat+start,chunk,MPI_INT,id,sub_comm);
		}
	}
#endif
	for (n=0; n < ntot_corners; n++) {
		//j = n / (x_N+1);
		//i = n % (x_N+1);
		j = masked_pixel_corner_j[n];
		i = masked_pixel_corner_i[n];
		corner_sourcepts[i][j][0] = defx_corners[n];
		corner_sourcepts[i][j][1] = defy_corners[n];
		//wtf << corner_pts[i][j][0] << " " << corner_pts[i][j][1] << " " << corner_sourcepts[i][j][0] << " " << corner_sourcepts[i][j][1] << " " << endl;
	}
	//wtf.close();
	int ii,jj;
	double u0,w0,mag;
	int subcell_index;
	for (n=0; n < ntot_cells; n++) {
		//n_cell = j*x_N+i;
		j = masked_pixels_j[n];
		i = masked_pixels_i[n];
		source_plane_triangle1_area[i][j] = area_tri1[n];
		source_plane_triangle2_area[i][j] = area_tri2[n];
		//if (i==176) cout << "AREAS (" << i << "," << j << "): " << area_tri1[n] << " " << area_tri2[n] << endl;
		twist_pts[i][j][0] = twistx[n];
		twist_pts[i][j][1] = twisty[n];
		twist_status[i][j] = twiststat[n];
		if (lens->split_high_mag_imgpixels) {
			mag = pixel_area/(area_tri1[n]+area_tri2[n]);
			//cout << "TRYING " << mag << " " << lens->image_pixel_data->surface_brightness[i][j] << endl;
			if ((lens->image_pixel_data->in_mask[i][j]) and ((mag > lens->imgpixel_himag_threshold) or (mag < lens->imgpixel_lomag_threshold)) and ((lens->image_pixel_data == NULL) or (lens->image_pixel_data->surface_brightness[i][j] > lens->imgpixel_sb_threshold))) {
				nsplits[i][j] = lens->default_imgpixel_nsplit;
				subcell_index = 0;
				for (ii=0; ii < nsplits[i][j]; ii++) {
					for (jj=0; jj < nsplits[i][j]; jj++) {
						u0 = ((double) (1+2*ii))/(2*nsplits[i][j]);
						w0 = ((double) (1+2*jj))/(2*nsplits[i][j]);
						subpixel_center_pts[i][j][subcell_index][0] = u0*corner_pts[i][j][0] + (1-u0)*corner_pts[i+1][j][0];
						subpixel_center_pts[i][j][subcell_index][1] = w0*corner_pts[i][j][1] + (1-w0)*corner_pts[i][j+1][1];
						subcell_index++;
					}
				}
				//cout << "Setting nsplit=" << lens->default_imgpixel_nsplit << " for pixel " << i << " " << j << " (sb=" << lens->image_pixel_data->surface_brightness[i][j] << ")" << endl;
			//} else {
				//cout << "NOPE, mag=" << mag << " sb=" << lens->image_pixel_data->surface_brightness[i][j] << " nsplit=" << nsplits[i][j] << endl; 
			} else {
				nsplits[i][j] = 1;
			}
		}
	}
	if ((lens->split_imgpixels) and (lens->split_high_mag_imgpixels)) setup_subpixel_ray_tracing_arrays(verbal);

	int mpi_chunk3, mpi_start3, mpi_end3;
	mpi_chunk3 = ntot_subpixels / lens->group_np;
	mpi_start3 = lens->group_id*mpi_chunk3;
	if (lens->group_id == lens->group_np-1) mpi_chunk3 += (ntot_subpixels % lens->group_np); // assign the remainder elements to the last mpi process
	mpi_end3 = mpi_start3 + mpi_chunk3;

	if (lens->split_imgpixels) {
		int n_subcell;
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif

			#pragma omp for private(i,j,k,n_subcell) schedule(dynamic)
			for (n_subcell=mpi_start3; n_subcell < mpi_end3; n_subcell++) {
				j = extended_mask_subcell_j[n_subcell];
				i = extended_mask_subcell_i[n_subcell];
				k = extended_mask_subcell_index[n_subcell];
				lens->find_sourcept(subpixel_center_pts[i][j][k],defx_subpixel_centers[n_subcell],defy_subpixel_centers[n_subcell],thread,imggrid_zfactors,imggrid_betafactors);
			}
		}
	}
#ifdef USE_MPI
	if ((lens->group_np > 1) and (lens->split_imgpixels)) {
		int id, chunk, start;
		for (id=0; id < lens->group_np; id++) {
			chunk = ntot_subpixels / lens->group_np;
			start = id*chunk;
			if (id == lens->group_np-1) chunk += (ntot_subpixels % lens->group_np); // assign the remainder elements to the last mpi process
			MPI_Bcast(defx_subpixel_centers+start,chunk,MPI_DOUBLE,id,sub_comm);
			MPI_Bcast(defy_subpixel_centers+start,chunk,MPI_DOUBLE,id,sub_comm);
		}
	}
	MPI_Comm_free(&sub_comm);
#endif
	if ((!lens->split_imgpixels) or (raytrace_pixel_centers)) {
		for (n=0; n < ntot_cells_emask; n++) {
			//n_cell = j*x_N+i;
			j = emask_pixels_j[n];
			i = emask_pixels_i[n];
			center_sourcepts[i][j][0] = defx_centers[n];
			center_sourcepts[i][j][1] = defy_centers[n];
		}
	}
	if (lens->split_imgpixels) {
		for (n=0; n < ntot_subpixels; n++) {
			j = extended_mask_subcell_j[n];
			i = extended_mask_subcell_i[n];
			k = extended_mask_subcell_index[n];
			//cout << "CHECKING2: " << defy_subpixel_centers[n] << " " << subpixel_center_sourcepts[i][j][k][1] << endl;
			//if (subpixel_center_sourcepts[i][j][k][0] != defx_subpixel_centers[n]) cout << "wrong defx: " << defx_subpixel_centers[n] << " " << subpixel_center_sourcepts[i][j][k][0] << endl;
			//if (subpixel_center_sourcepts[i][j][k][1] != defy_subpixel_centers[n]) cout << "wrong defy: " << defx_subpixel_centers[n] << " " << subpixel_center_sourcepts[i][j][k][1] << endl;
			subpixel_center_sourcepts[i][j][k][0] = defx_subpixel_centers[n];
			subpixel_center_sourcepts[i][j][k][1] = defy_subpixel_centers[n];
			//cout << "SRCPT: " << subpixel_center_sourcepts[i][j][k][0] << " " << subpixel_center_sourcepts[i][j][k][1] << endl;
		}
	}
}

void ImagePixelGrid::ray_trace_pixels()
{
	if (lens) {
		setup_ray_tracing_arrays();
		calculate_sourcepts_and_areas(true);
	}
}

void ImagePixelGrid::redo_lensing_calculations(const bool verbal)
{
	imggrid_zfactors = lens->reference_zfactors;
	imggrid_betafactors = lens->default_zsrc_beta_factors;
#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	if ((source_fit_mode==Cartesian_Source) or (source_fit_mode==Delaunay_Source)) n_active_pixels = 0;
	calculate_sourcepts_and_areas(true,verbal);

#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for ray-tracing image pixel grid: " << wtime << endl;
	}
#endif
}

void ImagePixelGrid::redo_lensing_calculations_corners() // this is used for analytic source mode with zooming when not using pixellated or shapelet sources
{
	// Update this so it uses the extended mask!! DO THIS!!!!!!!!
	imggrid_zfactors = lens->reference_zfactors;
	imggrid_betafactors = lens->default_zsrc_beta_factors;
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*(lens->group_comm), *(lens->mpi_group), &sub_comm);
#endif

#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	int i,j,n,n_cell,n_yp;
	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++) {
			if (lens->split_imgpixels) nsplits[i][j] = lens->default_imgpixel_nsplit; // default
		}
	}
	long int ntot_corners = (x_N+1)*(y_N+1);
	long int ntot_cells = x_N*y_N;
	double *defx_corners, *defy_corners;
	defx_corners = new double[ntot_corners];
	defy_corners = new double[ntot_corners];

	int mpi_chunk, mpi_start, mpi_end;
	mpi_chunk = ntot_corners / lens->group_np;
	mpi_start = lens->group_id*mpi_chunk;
	if (lens->group_id == lens->group_np-1) mpi_chunk += (ntot_corners % lens->group_np); // assign the remainder elements to the last mpi process
	mpi_end = mpi_start + mpi_chunk;

	#pragma omp parallel
	{
		int thread;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
		lensvector d1,d2,d3,d4;
		#pragma omp for private(n,i,j) schedule(dynamic)
		for (n=mpi_start; n < mpi_end; n++) {
			j = n / (x_N+1);
			i = n % (x_N+1);
			lens->find_sourcept(corner_pts[i][j],defx_corners[n],defy_corners[n],thread,imggrid_zfactors,imggrid_betafactors);
		}
	}
#ifdef USE_MPI
		#pragma omp master
		{
			int id, chunk, start;
			for (id=0; id < lens->group_np; id++) {
				chunk = ntot_corners / lens->group_np;
				start = id*chunk;
				if (id == lens->group_np-1) chunk += (ntot_corners % lens->group_np); // assign the remainder elements to the last mpi process
				MPI_Bcast(defx_corners+start,chunk,MPI_DOUBLE,id,sub_comm);
				MPI_Bcast(defy_corners+start,chunk,MPI_DOUBLE,id,sub_comm);
			}
		}
		#pragma omp barrier
#endif
	for (n=0; n < ntot_corners; n++) {
		j = n / (x_N+1);
		i = n % (x_N+1);
		corner_sourcepts[i][j][0] = defx_corners[n];
		corner_sourcepts[i][j][1] = defy_corners[n];
	}

#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for ray-tracing image pixel grid: " << wtime << endl;
	}
#endif
	delete[] defx_corners;
	delete[] defy_corners;
}

void ImagePixelGrid::load_data(ImagePixelData& pixel_data)
{
	max_sb = -1e30;
	int i,j;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			surface_brightness[i][j] = pixel_data.surface_brightness[i][j];
		}
	}
}

void ImagePixelGrid::plot_surface_brightness(string outfile_root, bool plot_residual, bool show_noise_thresh, bool plot_log)
{
	string sb_filename = outfile_root + ".dat";
	string x_filename = outfile_root + ".x";
	string y_filename = outfile_root + ".y";
	//string src_filename = outfile_root + "_srcpts.dat";

	ofstream pixel_image_file; lens->open_output_file(pixel_image_file,sb_filename);
	ofstream pixel_xvals; lens->open_output_file(pixel_xvals,x_filename);
	ofstream pixel_yvals; lens->open_output_file(pixel_yvals,y_filename);
	//ofstream pixel_src_file; lens->open_output_file(pixel_src_file,src_filename);
	pixel_image_file << setiosflags(ios::scientific);
	for (int i=0; i <= x_N; i++) {
		pixel_xvals << corner_pts[i][0][0] << endl;
	}
	for (int j=0; j <= y_N; j++) {
		pixel_yvals << corner_pts[0][j][1] << endl;
	}	
	int i,j;
	double residual;

	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((fit_to_data==NULL) or (fit_to_data[i][j])) {
				if (!plot_residual) {
					double sb = surface_brightness[i][j] + foreground_surface_brightness[i][j];
					//if (sb*0.0 != 0.0) die("WTF %g %g",surface_brightness[i][j],foreground_surface_brightness[i][j]);
					if (!plot_log) pixel_image_file << sb;
					else pixel_image_file << log(abs(sb));
				} else {
					double sb = surface_brightness[i][j] + foreground_surface_brightness[i][j];
					residual = lens->image_pixel_data->surface_brightness[i][j] - sb;
					if (show_noise_thresh) {
						if (abs(residual) >= lens->data_pixel_noise) pixel_image_file << residual;
						else pixel_image_file << "NaN";
					}
					else pixel_image_file << residual;
					//lens->find_sourcept(center_pts[i][j],center_sourcepts[i][j],0,imggrid_zfactors,imggrid_betafactors);
					//if (abs(residual) > 0.02) pixel_src_file << center_sourcepts[i][j][0] << " " << center_sourcepts[i][j][1] << " " << residual << endl;
				}
			} else {
				pixel_image_file << "NaN";
			}
			if (i < x_N-1) pixel_image_file << " ";
		}
		pixel_image_file << endl;
	}
	plot_sourcepts(outfile_root);
}

void ImagePixelGrid::plot_sourcepts(string outfile_root)
{
	string sp_filename = outfile_root + "_spt.dat";

	ofstream sourcepts_file; lens->open_output_file(sourcepts_file,sp_filename);
	sourcepts_file << setiosflags(ios::scientific);
	int i,j;
	double residual;

	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			//if ((fit_to_data==NULL) or (fit_to_data[i][j])) {
			if ((fit_to_data==NULL) or ((!lens->zero_sb_extended_mask_prior) and (lens->image_pixel_data->extended_mask[i][j])) or ((lens->zero_sb_extended_mask_prior) and (lens->image_pixel_data->in_mask[i][j]))) {
				sourcepts_file << center_sourcepts[i][j][0] << " " << center_sourcepts[i][j][1] << " " << center_pts[i][j][0] << " " << center_pts[i][j][1] << endl;
			}
		}
	}
}

void ImagePixelGrid::output_fits_file(string fits_filename, bool plot_residual)
{
#ifndef USE_FITS
	cout << "FITS capability disabled; QLens must be compiled with the CFITSIO library to write FITS files\n"; return;
#else
	int i,j,kk;
	fitsfile *outfptr;   // FITS file pointer, defined in fitsio.h
	int status = 0;   // CFITSIO status value MUST be initialized to zero!
	int bitpix = -64, naxis = 2;
	long naxes[2] = {x_N,y_N};
	double *pixels;
	string fits_filename_overwrite = "!" + fits_filename; // ensures that it overwrites an existing file of the same name

	if (!fits_create_file(&outfptr, fits_filename_overwrite.c_str(), &status))
	{
		if (!fits_create_img(outfptr, bitpix, naxis, naxes, &status))
		{
			if (naxis == 0) {
				die("Error: only 1D or 2D images are supported (dimension is %i)\n",naxis);
			} else {
				kk=0;
				long fpixel[naxis];
				for (kk=0; kk < naxis; kk++) fpixel[kk] = 1;
				pixels = new double[x_N];

				for (fpixel[1]=1, j=0; fpixel[1] <= naxes[1]; fpixel[1]++, j++)
				{
					for (i=0; i < x_N; i++) {
						if (!plot_residual) pixels[i] = surface_brightness[i][j] + foreground_surface_brightness[i][j];
						else pixels[i] = lens->image_pixel_data->surface_brightness[i][j] - surface_brightness[i][j] - foreground_surface_brightness[i][j];
					}
					fits_write_pix(outfptr, TDOUBLE, fpixel, naxes[0], pixels, &status);
				}
				delete[] pixels;
			}
			if (pixel_xlength==pixel_ylength)
				fits_write_key(outfptr, TDOUBLE, "PXSIZE", &pixel_xlength, "length of square pixels (in arcsec)", &status);
			if (lens->sim_pixel_noise != 0)
				fits_write_key(outfptr, TDOUBLE, "PXNOISE", &lens->sim_pixel_noise, "pixel surface brightness noise", &status);
			if ((lens->psf_width_x != 0) and (lens->psf_width_y==lens->psf_width_x) and (!lens->use_input_psf_matrix))
				fits_write_key(outfptr, TDOUBLE, "PSFSIG", &lens->psf_width_x, "Gaussian PSF width (dispersion, not FWHM)", &status);
			fits_write_key(outfptr, TDOUBLE, "ZSRC", &lens->source_redshift, "redshift of source galaxy", &status);
			if (lens->nlens > 0) {
				double zl = lens->lens_list[lens->primary_lens_number]->get_redshift();
				fits_write_key(outfptr, TDOUBLE, "ZLENS", &zl, "redshift of primary lens", &status);
			}

			if (lens->data_info != "") {
				string comment = "ql: " + lens->data_info;
				fits_write_comment(outfptr, comment.c_str(), &status);
			}
			if (lens->param_markers != "") {
				string param_markers_comma = lens->param_markers;
				// Commas are used as delimeter in FITS file so spaces won't get lost when reading it in
				for (size_t i = 0; i < param_markers_comma.size(); ++i) {
					 if (param_markers_comma[i] == ' ') {
						  param_markers_comma.replace(i, 1, ",");
					 }
				}

				string comment = "mk: " + param_markers_comma;
				fits_write_comment(outfptr, comment.c_str(), &status);
			}
		}
		fits_close_file(outfptr, &status);
	} 

	if (status) fits_report_error(stderr, status); // print any error message
#endif
}

void ImagePixelGrid::set_fit_window(ImagePixelData& pixel_data, const bool raytrace)
{
	if ((x_N != pixel_data.npixels_x) or (y_N != pixel_data.npixels_y)) {
		warn("Number of data pixels does not match specified number of image pixels; cannot activate fit window");
		return;
	}
	int i,j;
	if (fit_to_data==NULL) {
		fit_to_data = new bool*[x_N];
		for (i=0; i < x_N; i++) fit_to_data[i] = new bool[y_N];
	}
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			fit_to_data[i][j] = pixel_data.in_mask[i][j];
			mapped_cartesian_srcpixels[i][j].clear();
			mapped_delaunay_srcpixels[i][j].clear();
		}
	}
	//double mask_min_r = 1e30;
	//for (i=0; i < x_N; i++) {
		//for (j=0; j < y_N; j++) {
			//if (pixel_data.in_mask[i][j]) {
				//double r = sqrt(SQR(center_pts[i][j][0]) + SQR(center_pts[i][j][1]));
				//if (r < mask_min_r) mask_min_r = r;
			//}
		//}
	//}
	//if ((lens) and (lens->mpi_id==0)) cout << "HACK: mask_min_r=" << mask_min_r << endl;

	if (lens) {
		setup_ray_tracing_arrays();
		if ((raytrace) or (lens->split_high_mag_imgpixels)) calculate_sourcepts_and_areas(true);
	}
}


void ImagePixelGrid::include_all_pixels()
{
	int i,j;
	if (fit_to_data==NULL) {
		fit_to_data = new bool*[x_N];
		for (i=0; i < x_N; i++) fit_to_data[i] = new bool[y_N];
	}
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			fit_to_data[i][j] = true;
		}
	}
}

void ImagePixelGrid::activate_extended_mask()
{
	int i,j;
	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++) {
			fit_to_data[i][j] = lens->image_pixel_data->extended_mask[i][j];
		}
	}
}

void ImagePixelGrid::activate_foreground_mask()
{
	int i,j;
	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++) {
			fit_to_data[i][j] = lens->image_pixel_data->foreground_mask[i][j];
		}
	}
}

void ImagePixelGrid::deactivate_extended_mask()
{
	int i,j;
	//int n=0, m=0;
	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++) {
			fit_to_data[i][j] = lens->image_pixel_data->in_mask[i][j];
			//if (fit_to_data[i][j]) n++;
			//if (lens->image_pixel_data->extended_mask[i][j]) m++;
		}
	}
	//cout << "NFIT: " << n << endl;
	//cout << "NEXT: " << m << endl;
}

void ImagePixelGrid::set_nsplits(ImagePixelData *pixel_data, const int default_nsplit, const int emask_nsplit, const bool split_pixels)
{
	int i,j,ii,jj,nsplit,subcell_index;
	double u0,w0;
	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++) {
			if (split_pixels) {
				if ((fit_to_data) and (pixel_data)) {
					if (pixel_data->in_mask[i][j]) nsplit = default_nsplit;
					else nsplit = emask_nsplit; // so extended mask pixels don't get split (make this customizable by user?)
				} else {
					nsplit = default_nsplit;
				}
				nsplits[i][j] = nsplit;
				subcell_index = 0;
				for (ii=0; ii < nsplit; ii++) {
					for (jj=0; jj < nsplit; jj++) {
						u0 = ((double) (1+2*ii))/(2*nsplit);
						w0 = ((double) (1+2*jj))/(2*nsplit);
						subpixel_center_pts[i][j][subcell_index][0] = u0*corner_pts[i][j][0] + (1-u0)*corner_pts[i+1][j][0];
						subpixel_center_pts[i][j][subcell_index][1] = w0*corner_pts[i][j][1] + (1-w0)*corner_pts[i][j+1][1];
						subcell_index++;
					}
				}
			} else {
				nsplits[i][j] = 1;
			}
		}
	}
}

bool ImagePixelData::test_if_in_fit_region(const double& x, const double& y)
{
	// it would be faster to just use division to figure out which pixel it's in, but this is good enough
	int i,j;
	for (j=0; j <= npixels_y; j++) {
		if ((yvals[j] <= y) and (yvals[j+1] > y)) {
			for (i=0; i <= npixels_x; i++) {
				if ((xvals[i] <= x) and (xvals[i+1] > x)) {
					if (in_mask[i][j] == true) return true;
					else break;
				}
			}
		}
	}
	return false;
}

double ImagePixelGrid::calculate_signal_to_noise(const double& pixel_noise_sig, double& total_signal)
{
	// NOTE: This function should be called *before* adding noise to the image.
	double sbmax=-1e30;
	static const double signal_threshold_frac = 1e-1;
	int i,j;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if (surface_brightness[i][j] > sbmax) sbmax = surface_brightness[i][j];
		}
	}
	double signal_mean=0;
	int npixels=0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if (surface_brightness[i][j] > signal_threshold_frac*sbmax) {
				signal_mean += surface_brightness[i][j];
				npixels++;
			}
		}
	}
	total_signal = signal_mean * pixel_xlength * pixel_ylength;
	if (npixels > 0) signal_mean /= npixels;
	return signal_mean / pixel_noise_sig;
}

void ImagePixelGrid::add_pixel_noise(const double& pixel_noise_sig)
{
	if (surface_brightness == NULL) die("surface brightness pixel map has not been loaded");
	int i,j;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			surface_brightness[i][j] += pixel_noise_sig*lens->NormalDeviate();
		}
	}
	pixel_noise = pixel_noise_sig;
}

void ImagePixelGrid::find_optimal_sourcegrid(double& sourcegrid_xmin, double& sourcegrid_xmax, double& sourcegrid_ymin, double& sourcegrid_ymax, const double &sourcegrid_limit_xmin, const double &sourcegrid_limit_xmax, const double &sourcegrid_limit_ymin, const double& sourcegrid_limit_ymax)
{
	if (surface_brightness == NULL) die("surface brightness pixel map has not been loaded");
	bool use_noise_threshold = true;
	if (lens->noise_threshold <= 0) use_noise_threshold = false;
	double threshold = lens->noise_threshold*pixel_noise;
	int i,j,k,nsp;
	sourcegrid_xmin=1e30;
	sourcegrid_xmax=-1e30;
	sourcegrid_ymin=1e30;
	sourcegrid_ymax=-1e30;
	int ii,jj,il,ih,jl,jh,nn;
	double sbavg;
	double xsavg, ysavg;
	static const int window_size_for_sbavg = 0;
	bool resize_grid;
	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++) {
			if (fit_to_data[i][j]) {
				resize_grid = true;
				if (use_noise_threshold) {
					sbavg=0;
					nn=0;
					il = i - window_size_for_sbavg;
					ih = i + window_size_for_sbavg;
					jl = j - window_size_for_sbavg;
					jh = j + window_size_for_sbavg;
					if (il<0) il=0;
					if (ih>x_N-1) ih=x_N-1;
					if (jl<0) jl=0;
					if (jh>y_N-1) jh=y_N-1;
					for (ii=il; ii <= ih; ii++) {
						for (jj=jl; jj <= jh; jj++) {
							sbavg += surface_brightness[ii][jj];
							nn++;
						}
					}
					sbavg /= nn;
					if (sbavg <= threshold) resize_grid = false;
				}
				if (resize_grid) {
					if (!lens->split_imgpixels) {
						xsavg = center_sourcepts[i][j][0];
						ysavg = center_sourcepts[i][j][1];
					} else {
						xsavg=ysavg=0;
						nsp = INTSQR(nsplits[i][j]);
						for (k=0; k < nsp; k++) {
							xsavg += subpixel_center_sourcepts[i][j][k][0];
							ysavg += subpixel_center_sourcepts[i][j][k][1];
						}
						xsavg /= nsp;
						ysavg /= nsp;
					}

					if (xsavg < sourcegrid_xmin) {
						if (xsavg > sourcegrid_limit_xmin) sourcegrid_xmin = xsavg;
						else if (sourcegrid_xmin > sourcegrid_limit_xmin) sourcegrid_xmin = sourcegrid_limit_xmin;
					}
					if (xsavg > sourcegrid_xmax) {
						if (xsavg < sourcegrid_limit_xmax) sourcegrid_xmax = xsavg;
						else if (sourcegrid_xmax < sourcegrid_limit_xmax) sourcegrid_xmax = sourcegrid_limit_xmax;
					}
					if (ysavg < sourcegrid_ymin) {
						if (ysavg > sourcegrid_limit_ymin) sourcegrid_ymin = ysavg;
						else if (sourcegrid_ymin > sourcegrid_limit_ymin) sourcegrid_ymin = sourcegrid_limit_ymin;
					}
					if (ysavg > sourcegrid_ymax) {
						if (ysavg < sourcegrid_limit_ymax) sourcegrid_ymax = ysavg;
						else if (sourcegrid_ymax < sourcegrid_limit_ymax) sourcegrid_ymax = sourcegrid_limit_ymax;
					}
				}
			}
		}
	}
	// Now let's make the box slightly wider just to be safe
	double xwidth_adj = 0.1*(sourcegrid_xmax-sourcegrid_xmin);
	double ywidth_adj = 0.1*(sourcegrid_ymax-sourcegrid_ymin);
	sourcegrid_xmin -= xwidth_adj/2;
	sourcegrid_xmax += xwidth_adj/2;
	sourcegrid_ymin -= ywidth_adj/2;
	sourcegrid_ymax += ywidth_adj/2;
}

void ImagePixelGrid::find_optimal_shapelet_scale(double& scale, double& xcenter, double& ycenter, double& recommended_nsplit, const bool verbal, double& sig, double& scaled_maxdist)
{
	//string sp_filename = "wtf_spt.dat";

	//ofstream sourcepts_file; lens->open_output_file(sourcepts_file,sp_filename);
	//sourcepts_file << setiosflags(ios::scientific);

	double xcavg, ycavg;
	double totsurf;
	double area, min_area = 1e30, max_area = -1e30;
	double xcmin, ycmin, sb;
	double xsavg, ysavg;
	int i,j,k,nsp;
	double rsq, rsqavg;
	sig = 1e30;
	int npts=0, npts_old, iter=0;
	//ofstream wtf("wtf.dat");
	do {
		// will use 3-sigma clipping to estimate center and dispersion of source
		npts_old = npts;
		xcavg = 0;
		ycavg = 0;
		totsurf = 0;
		npts=0;
		for (i=0; i < x_N; i++) {
			for (j=0; j < y_N; j++) {
				//if (foreground_surface_brightness[i][i] != 0) die("YEAH! %g",foreground_surface_brightness[i][j]);
				if (((fit_to_data==NULL) or (lens->image_pixel_data->in_mask[i][j])) and (abs(sb = surface_brightness[i][j] - foreground_surface_brightness[i][j]) > 5*pixel_noise)) {
					//xsavg = (corner_sourcepts[i][j][0] + corner_sourcepts[i+1][j][0] + corner_sourcepts[i+1][j][0] + corner_sourcepts[i+1][j+1][0]) / 4;
					//ysavg = (corner_sourcepts[i][j][1] + corner_sourcepts[i+1][j][1] + corner_sourcepts[i+1][j][1] + corner_sourcepts[i+1][j+1][1]) / 4;
					// You repeat this code three times in this function! Store things in arrays and GET RID OF THE REDUNDANCIES!!!! IT'S UGLY.
					if (!lens->split_imgpixels) {
						xsavg = center_sourcepts[i][j][0];
						ysavg = center_sourcepts[i][j][1];
					} else {
						xsavg=ysavg=0;
						nsp = INTSQR(nsplits[i][j]);
						for (k=0; k < nsp; k++) {
							xsavg += subpixel_center_sourcepts[i][j][k][0];
							ysavg += subpixel_center_sourcepts[i][j][k][1];
						}
						xsavg /= nsp;
						ysavg /= nsp;
					}
					//cout << "HI (" << xsavg << "," << ysavg << ") vs (" << center_sourcepts[i][j][0] << "," << center_sourcepts[i][j][1] << ")" << endl;
					area = (source_plane_triangle1_area[i][j] + source_plane_triangle2_area[i][j]);
					rsq = SQR(xsavg - xcavg) + SQR(ysavg - ycavg);
					if ((iter==0) or (sqrt(rsq) < 3*sig)) {
						xcavg += area*abs(sb)*xsavg;
						ycavg += area*abs(sb)*ysavg;
						totsurf += area*abs(sb);
						npts++;
					}
					//wtf << center_sourcepts[i][j][0] << " " << center_sourcepts[i][j][1] << endl;
				}
			}
		}
		//wtf.close();
		xcavg /= totsurf;
		ycavg /= totsurf;
		rsqavg=0;
		// NOTE: the approx. sigma found below will be inflated a bit due to the effect of the PSF (but that's probably ok)
		for (i=0; i < x_N; i++) {
			for (j=0; j < y_N; j++) {
				if (((fit_to_data==NULL) or (lens->image_pixel_data->in_mask[i][j])) and (abs(sb = surface_brightness[i][j] - foreground_surface_brightness[i][j]) > 5*pixel_noise)) {
					//xsavg = (corner_sourcepts[i][j][0] + corner_sourcepts[i+1][j][0] + corner_sourcepts[i+1][j][0] + corner_sourcepts[i+1][j+1][0]) / 4;
					//ysavg = (corner_sourcepts[i][j][1] + corner_sourcepts[i+1][j][1] + corner_sourcepts[i+1][j][1] + corner_sourcepts[i+1][j+1][1]) / 4;
					if (!lens->split_imgpixels) {
						xsavg = center_sourcepts[i][j][0];
						ysavg = center_sourcepts[i][j][1];
					} else {
						xsavg=ysavg=0;
						nsp = INTSQR(nsplits[i][j]);
						for (k=0; k < nsp; k++) {
							xsavg += subpixel_center_sourcepts[i][j][k][0];
							ysavg += subpixel_center_sourcepts[i][j][k][1];
						}
						xsavg /= nsp;
						ysavg /= nsp;
					}

					area = (source_plane_triangle1_area[i][j] + source_plane_triangle2_area[i][j]);
					rsq = SQR(xsavg - xcavg) + SQR(ysavg - ycavg);
					if ((iter==0) or (sqrt(rsq) < 3*sig)) {
						rsqavg += area*abs(sb)*rsq;
					}
				}
			}
		}
		//cout << "rsqavg=" << rsqavg << " totsurf=" << totsurf << endl;
		rsqavg /= totsurf;
		sig = sqrt(abs(rsqavg));
		//cout << "Iteration " << iter << ": sig=" << sig << ", xc=" << xcavg << ", yc=" << ycavg << ", npts=" << npts << endl;
		iter++;
	} while ((iter < 6) and (npts != npts_old));
	xcenter = xcavg;
	ycenter = ycavg;

	int ntot=0, nout=0;
	double xd,xmax=-1e30;
	double yd,ymax=-1e30;
	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++) {
			//if (((fit_to_data==NULL) or (fit_to_data[i][j])) and (abs(sb) > 5*pixel_noise)) {
			if ((fit_to_data==NULL) or ((!lens->zero_sb_extended_mask_prior) and (lens->image_pixel_data->extended_mask[i][j])) or ((lens->zero_sb_extended_mask_prior) and (lens->image_pixel_data->in_mask[i][j]))) {
				//xsavg = (corner_sourcepts[i][j][0] + corner_sourcepts[i+1][j][0] + corner_sourcepts[i+1][j][0] + corner_sourcepts[i+1][j+1][0]) / 4;
				//ysavg = (corner_sourcepts[i][j][1] + corner_sourcepts[i+1][j][1] + corner_sourcepts[i+1][j][1] + corner_sourcepts[i+1][j+1][1]) / 4;
				if (!lens->split_imgpixels) {
					xsavg = center_sourcepts[i][j][0];
					ysavg = center_sourcepts[i][j][1];
				} else {
					xsavg=ysavg=0;
					nsp = INTSQR(nsplits[i][j]);
					for (k=0; k < nsp; k++) {
						xsavg += subpixel_center_sourcepts[i][j][k][0];
						ysavg += subpixel_center_sourcepts[i][j][k][1];
					}
					xsavg /= nsp;
					ysavg /= nsp;
				}

				xd = abs(xsavg-xcavg);
				yd = abs(ysavg-ycavg);

				//double ri = abs(sqrt(SQR(center_pts[i][j][0]-0.01)+SQR(center_pts[i][j][1])));
				//if (ri > 0.6) {
				if (xd > xmax) xmax = xd;
				if (yd > ymax) ymax = yd;
					//sourcepts_file << xsavg << " " << ysavg << " " << center_pts[i][j][0] << " " << center_pts[i][j][1] << " " << xd << " " << yd << endl;
				//}
				if ((lens->image_pixel_data->in_mask[i][j]) and (abs(surface_brightness[i][j] - foreground_surface_brightness[i][j]) > 5*pixel_noise)) {
					ntot++;
					rsq = SQR(xd) + SQR(yd);
					if (sqrt(rsq) > 2*sig) {
						nout++;
					}
				}
			}
		}
	}
	double fout = nout / ((double) ntot);
	if ((verbal) and (lens->mpi_id==0)) cout << "Fraction of 2-sigma outliers for shapelets: " << fout << endl;
	double maxdist = dmax(xmax,ymax);

	int nn = lens->get_shapelet_nn();
	scaled_maxdist = lens->shapelet_window_scaling*maxdist;
	if (lens->shapelet_scale_mode==0) {
		scale = sig; // uses the dispersion of source SB to set scale (WARNING: might not cover all of lensed pixels in mask if n_shapelets is too small!!)
	} else if (lens->shapelet_scale_mode==1) {
		scale = 1.000001*scaled_maxdist/sqrt(nn); // uses outermost pixel in extended mask to set scale
		if (scale > sig) scale = sig; // if the above scale is bigger than ray-traced source scale (sig), then just set scale = sig (otherwise source will be under-resolved)
	}
	if (scale > lens->shapelet_max_scale) scale = lens->shapelet_max_scale;

	int ii, jj, il, ih, jl, jh;
	const double window_size_for_srcarea = 1;
	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++) {
			sb = surface_brightness[i][j] - foreground_surface_brightness[i][j];
			//if (((fit_to_data==NULL) or (fit_to_data[i][j])) and (abs(sb) > 5*pixel_noise)) {
			if (((fit_to_data==NULL) or (lens->image_pixel_data->in_mask[i][j])) and (abs(sb) > 5*pixel_noise)) {
				il = i - window_size_for_srcarea;
				ih = i + window_size_for_srcarea;
				jl = j - window_size_for_srcarea;
				jh = j + window_size_for_srcarea;
				if (il<0) il=0;
				if (ih>x_N-1) ih=x_N-1;
				if (jl<0) jl=0;
				if (jh>y_N-1) jh=y_N-1;
				area=0;
				for (ii=il; ii <= ih; ii++) {
					for (jj=jl; jj <= jh; jj++) {
						area += (source_plane_triangle1_area[ii][jj] + source_plane_triangle2_area[ii][jj]);
					}
				}
				if (area < min_area) {
					min_area = area;
					xcmin = center_pts[i][j][0];
					ycmin = center_pts[i][j][1];
				}
				if (area > max_area) {
					max_area = area;
				}

			}
		}
	}

	double minscale_res = sqrt(min_area);
	recommended_nsplit = 2*sqrt(max_area*nn)/sig; // this is so the smallest source fluctuations get at least 2x2 ray tracing coverage
	int recommended_nn;
	recommended_nn = ((int) (SQR(sig / minscale_res))) + 1;
	if ((verbal) and (lens->mpi_id==0)) {
		cout << "expected minscale_res=" << minscale_res << ", found at (x=" << xcmin << ",y=" << ycmin << ") recommended_nn=" << recommended_nn << endl;
		cout << "number of splittings should be at least " << recommended_nsplit << " to capture all source fluctuations" << endl;
		cout << "outermost ray-traced source pixel distance: " << scaled_maxdist << endl;
	}
}

void ImagePixelGrid::fill_surface_brightness_vector()
{
	int column_j = 0;
	int i,j;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((maps_to_source_pixel[i][j]) and ((fit_to_data==NULL) or (fit_to_data[i][j]))) {
				lens->image_surface_brightness[column_j++] = surface_brightness[i][j];
			}
		}
	}
}

void ImagePixelGrid::plot_grid(string filename, bool show_inactive_pixels)
{
	int i,j;
	string grid_filename = filename + ".pixgrid";
	string center_filename = filename + ".pixcenters";
	ofstream gridfile;
	lens->open_output_file(gridfile,grid_filename);
	ofstream centerfile;
	lens->open_output_file(centerfile,center_filename);
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((!fit_to_data) or (fit_to_data[i][j])) {
				//cout << "WHAZZUP " << i << " " << j << endl;
				//if ((show_inactive_pixels) or (maps_to_source_pixel[i][j])) {
					gridfile << corner_sourcepts[i][j][0] << " " << corner_sourcepts[i][j][1] << " " << corner_pts[i][j][0] << " " << corner_pts[i][j][1] << endl;
					gridfile << corner_sourcepts[i+1][j][0] << " " << corner_sourcepts[i+1][j][1] << " " << corner_pts[i+1][j][0] << " " << corner_pts[i+1][j][1] << endl;
					gridfile << corner_sourcepts[i+1][j+1][0] << " " << corner_sourcepts[i+1][j+1][1] << " " << corner_pts[i+1][j+1][0] << " " << corner_pts[i+1][j+1][1] << endl;
					gridfile << corner_sourcepts[i][j+1][0] << " " << corner_sourcepts[i][j+1][1] << " " << corner_pts[i][j+1][0] << " " << corner_pts[i][j+1][1] << endl;
					gridfile << corner_sourcepts[i][j][0] << " " << corner_sourcepts[i][j][1] << " " << corner_pts[i][j][0] << " " << corner_pts[i][j][1] << endl;
					gridfile << endl;
					centerfile << center_sourcepts[i][j][0] << " " << center_sourcepts[i][j][1] << " " << center_pts[i][j][0] << " " << center_pts[i][j][1] << endl;
				//}
			}
		}
	}
}

void ImagePixelGrid::find_optimal_sourcegrid_npixels(double pixel_fraction, double srcgrid_xmin, double srcgrid_xmax, double srcgrid_ymin, double srcgrid_ymax, int& nsrcpixel_x, int& nsrcpixel_y, int& n_expected_active_pixels)
{
	int i,j,count=0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((fit_to_data==NULL) or (fit_to_data[i][j])) {
				if ((center_sourcepts[i][j][0] > srcgrid_xmin) and (center_sourcepts[i][j][0] < srcgrid_xmax) and (center_sourcepts[i][j][1] > srcgrid_ymin) and (center_sourcepts[i][j][1] < srcgrid_ymax)) {
					count++;
				}
			}
		}
	}
	double dx = srcgrid_xmax-srcgrid_xmin;
	double dy = srcgrid_ymax-srcgrid_ymin;
	nsrcpixel_x = (int) sqrt(pixel_fraction*count*dx/dy);
	nsrcpixel_y = (int) nsrcpixel_x*dy/dx;
	n_expected_active_pixels = count;
}

void ImagePixelGrid::find_optimal_firstlevel_sourcegrid_npixels(double srcgrid_xmin, double srcgrid_xmax, double srcgrid_ymin, double srcgrid_ymax, int& nsrcpixel_x, int& nsrcpixel_y, int& n_expected_active_pixels)
{
	// this algorithm assumes an adaptive grid, so that higher magnification regions will be subgridded
	// it really doesn't seem to work well though...
	double lowest_magnification = 1e30;
	double average_magnification = 0;
	int i,j,count=0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((fit_to_data==NULL) or (fit_to_data[i][j])) {
				if ((center_sourcepts[i][j][0] > srcgrid_xmin) and (center_sourcepts[i][j][0] < srcgrid_xmax) and (center_sourcepts[i][j][1] > srcgrid_ymin) and (center_sourcepts[i][j][1] < srcgrid_ymax)) {
					count++;
				}
			}
		}
	}

	double pixel_area, source_lowlevel_pixel_area, dx, dy, srcgrid_area, srcgrid_firstlevel_npixels;
	pixel_area = pixel_xlength * pixel_ylength;
	source_lowlevel_pixel_area = pixel_area / (1.3*lens->pixel_magnification_threshold);
	dx = srcgrid_xmax-srcgrid_xmin;
	dy = srcgrid_ymax-srcgrid_ymin;
	srcgrid_area = dx*dy;
	srcgrid_firstlevel_npixels = dx*dy/source_lowlevel_pixel_area;
	nsrcpixel_x = (int) sqrt(srcgrid_firstlevel_npixels*dx/dy);
	nsrcpixel_y = (int) nsrcpixel_x*dy/dx;
	int srcgrid_npixels = nsrcpixel_x*nsrcpixel_y;
	n_expected_active_pixels = count;
}

/*
void ImagePixelGrid::assign_required_data_pixels(double srcgrid_xmin, double srcgrid_xmax, double srcgrid_ymin, double srcgrid_ymax, int& count, ImagePixelData *data_in)
{
	int i,j;
	count=0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((center_sourcepts[i][j][0] > srcgrid_xmin) and (center_sourcepts[i][j][0] < srcgrid_xmax) and (center_sourcepts[i][j][1] > srcgrid_ymin) and (center_sourcepts[i][j][1] < srcgrid_ymax)) {
				data_in->in_mask[i][j] = true;
				count++;
			}
			else {
				data_in->in_mask[i][j] = false;
			}
		}
	}
}
*/

int ImagePixelGrid::count_nonzero_source_pixel_mappings_cartesian()
{
	int tot=0;
	int i,j,k,img_index;
	for (img_index=0; img_index < lens->image_npixels; img_index++) {
		i = lens->active_image_pixel_i[img_index];
		j = lens->active_image_pixel_j[img_index];
		for (k=0; k < mapped_cartesian_srcpixels[i][j].size(); k++) {
			if (mapped_cartesian_srcpixels[i][j][k] != NULL) tot++;
		}
		//tot += mapped_cartesian_srcpixels[i][j].size();
	}
	return tot;
}

int ImagePixelGrid::count_nonzero_source_pixel_mappings_delaunay()
{
	int tot=0;
	int i,j,k,img_index;
	for (img_index=0; img_index < lens->image_npixels; img_index++) {
		i = lens->active_image_pixel_i[img_index];
		j = lens->active_image_pixel_j[img_index];
		for (k=0; k < mapped_delaunay_srcpixels[i][j].size(); k++) {
			if (mapped_delaunay_srcpixels[i][j][k] != -1) tot++;
		}
		//tot += mapped_delaunay_srcpixels[i][j].size();
	}
	return tot;
}

void ImagePixelGrid::assign_image_mapping_flags(const bool delaunay)
{
	int i,j;
	n_active_pixels = 0;
	n_high_sn_pixels = 0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if (delaunay) mapped_delaunay_srcpixels[i][j].clear();
			else mapped_cartesian_srcpixels[i][j].clear();
			maps_to_source_pixel[i][j] = false;
		}
	}
	if ((!delaunay) and (ray_tracing_method == Area_Overlap)) // Delaunay grid does not support overlap ray tracing
	{
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			lensvector *corners[4];
			#pragma omp for private(i,j,corners) schedule(dynamic)
			for (j=0; j < y_N; j++) {
				for (i=0; i < x_N; i++) {
					if ((fit_to_data == NULL) or (fit_to_data[i][j])) {
						corners[0] = &corner_sourcepts[i][j];
						corners[1] = &corner_sourcepts[i][j+1];
						corners[2] = &corner_sourcepts[i+1][j];
						corners[3] = &corner_sourcepts[i+1][j+1];
						if (source_pixel_grid->assign_source_mapping_flags_overlap(corners,&twist_pts[i][j],twist_status[i][j],mapped_cartesian_srcpixels[i][j],thread)==true) {
							maps_to_source_pixel[i][j] = true;
							#pragma omp atomic
							n_active_pixels++;
							if ((fit_to_data != NULL) and (fit_to_data[i][j]) and (lens->image_pixel_data->high_sn_pixel[i][j])) n_high_sn_pixels++;
						} else
							maps_to_source_pixel[i][j] = false;
					}
				}
			}
		}
	}
	else if (ray_tracing_method == Interpolate)
	{
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			if (lens->split_imgpixels) {
				int nsubpix,subcell_index;
				bool maps_to_something;
				#pragma omp for private(i,j,nsubpix,subcell_index,maps_to_something) schedule(dynamic)
				for (j=0; j < y_N; j++) {
					for (i=0; i < x_N; i++) {
						if ((fit_to_data == NULL) or (fit_to_data[i][j])) {
							nsubpix = INTSQR(nsplits[i][j]);
							maps_to_something = false;
							for (subcell_index=0; subcell_index < nsubpix; subcell_index++)
							{
								if ((delaunay) and (delaunay_srcgrid->assign_source_mapping_flags(subpixel_center_sourcepts[i][j][subcell_index],mapped_delaunay_srcpixels[i][j],i,j,thread)==true)) {
									maps_to_something = true;
									subpixel_maps_to_srcpixel[i][j][subcell_index] = true;
								} else if ((!delaunay) and (source_pixel_grid->assign_source_mapping_flags_interpolate(subpixel_center_sourcepts[i][j][subcell_index],mapped_cartesian_srcpixels[i][j],thread,i,j)==true)) {
									maps_to_something = true;
									subpixel_maps_to_srcpixel[i][j][subcell_index] = true;
								} else {
									subpixel_maps_to_srcpixel[i][j][subcell_index] = false;
								}
							}
							if (maps_to_something==true) {
								maps_to_source_pixel[i][j] = true;
								#pragma omp atomic
								n_active_pixels++;
								if ((fit_to_data != NULL) and (fit_to_data[i][j]) and (lens->image_pixel_data->high_sn_pixel[i][j])) {
									#pragma omp atomic
									n_high_sn_pixels++;
								}
							} else maps_to_source_pixel[i][j] = false;
						}
					}
				}
			} else {
				#pragma omp for private(i,j) schedule(dynamic)	
				for (j=0; j < y_N; j++) {
					for (i=0; i < x_N; i++) {
						if ((fit_to_data == NULL) or (fit_to_data[i][j])) {
							if ((delaunay) and (delaunay_srcgrid->assign_source_mapping_flags(center_sourcepts[i][j],mapped_delaunay_srcpixels[i][j],i,j,thread)==true)) {
								maps_to_source_pixel[i][j] = true;
								#pragma omp atomic
								n_active_pixels++;
								if ((fit_to_data != NULL) and (fit_to_data[i][j]) and (lens->image_pixel_data->high_sn_pixel[i][j])) {
									#pragma omp atomic
									n_high_sn_pixels++;
								}
							} else if ((!delaunay) and (source_pixel_grid->assign_source_mapping_flags_interpolate(center_sourcepts[i][j],mapped_cartesian_srcpixels[i][j],thread,i,j)==true)) {
								maps_to_source_pixel[i][j] = true;
								#pragma omp atomic
								n_active_pixels++;
								if ((fit_to_data != NULL) and (fit_to_data[i][j]) and (lens->image_pixel_data->high_sn_pixel[i][j])) {
									#pragma omp atomic
									n_high_sn_pixels++;
								}
							} else {
								maps_to_source_pixel[i][j] = false;
							}
						}
					}
				}
			}
		}
	}
}

void ImagePixelGrid::find_surface_brightness(const bool foreground_only, const bool lensed_sources_only)
{
	imggrid_zfactors = lens->reference_zfactors;
	imggrid_betafactors = lens->default_zsrc_beta_factors;
#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int i,j;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			surface_brightness[i][j] = 0;
		}
	}
	if ((source_fit_mode == Cartesian_Source) or (source_fit_mode == Delaunay_Source)) {
		bool at_least_one_foreground_src = false;
		bool at_least_one_lensed_src = false;
		for (int k=0; k < lens->n_sb; k++) {
			if (!lens->sb_list[k]->is_lensed) {
				at_least_one_foreground_src = true;
			} else {
				at_least_one_lensed_src = true;
			}
		}
		if ((foreground_only) and (!at_least_one_foreground_src)) return;

		if ((source_fit_mode == Cartesian_Source) and (ray_tracing_method == Area_Overlap)) {
			lensvector **corners = new lensvector*[4];
			for (j=0; j < y_N; j++) {
				for (i=0; i < x_N; i++) {
					//surface_brightness[i][j] = 0;
					corners[0] = &corner_sourcepts[i][j];
					corners[1] = &corner_sourcepts[i][j+1];
					corners[2] = &corner_sourcepts[i+1][j];
					corners[3] = &corner_sourcepts[i+1][j+1];
					if (!foreground_only) surface_brightness[i][j] = source_pixel_grid->find_lensed_surface_brightness_overlap(corners,&twist_pts[i][j],twist_status[i][j],0);
					if ((at_least_one_foreground_src) and (!lensed_sources_only)) {
						for (int k=0; k < lens->n_sb; k++) {
							if (!lens->sb_list[k]->is_lensed) {
								if (!lens->sb_list[k]->zoom_subgridding) {
									surface_brightness[i][j] += lens->sb_list[k]->surface_brightness(center_pts[i][j][0],center_pts[i][j][1]);
								}
								else surface_brightness[i][j] += lens->sb_list[k]->surface_brightness_zoom(center_pts[i][j],corner_pts[i][j],corner_pts[i+1][j],corner_pts[i][j+1],corner_pts[i+1][j+1]);
							}
						}
					}
				}
			}
			delete[] corners;
		} else { // use interpolation to get surface brightness
			if (lens->split_imgpixels) {
				#pragma omp parallel
				{
					int thread;
#ifdef USE_OPENMP
					thread = omp_get_thread_num();
#else
					thread = 0;
#endif
					//int ii,jj,nsplit;
					//double u0, w0, sb;
					lensvector corner1, corner2, corner3, corner4;
					double subpixel_xlength, subpixel_ylength;
					double sb;

					int nsubpix,subcell_index;
					lensvector *center_srcpt, *center_pt;
					//#pragma omp for private(i,j,ii,jj,nsplit,u0,w0,sb) schedule(dynamic)
					#pragma omp for private(i,j,nsubpix,subcell_index,center_pt,center_srcpt,subpixel_xlength,subpixel_ylength,corner1,corner2,corner3,corner4,sb) schedule(dynamic)
					for (j=0; j < y_N; j++) {
						for (i=0; i < x_N; i++) {
							//surface_brightness[i][j] = 0;
							if ((fit_to_data == NULL) or (fit_to_data[i][j])) {
								sb=0;

								nsubpix = INTSQR(nsplits[i][j]);
								center_srcpt = subpixel_center_sourcepts[i][j];
								center_pt = subpixel_center_pts[i][j];
								for (subcell_index=0; subcell_index < nsubpix; subcell_index++) {
									if (!foreground_only) {
										if (source_fit_mode==Delaunay_Source) sb += delaunay_srcgrid->find_lensed_surface_brightness(center_srcpt[subcell_index],i,j,thread);
										else if (source_fit_mode==Cartesian_Source) sb += source_pixel_grid->find_lensed_surface_brightness_interpolate(center_srcpt[subcell_index],thread);
									}
									if ((at_least_one_foreground_src) and (!lensed_sources_only)) {
										for (int k=0; k < lens->n_sb; k++) {
											if (!lens->sb_list[k]->is_lensed) {
												if (!lens->sb_list[k]->zoom_subgridding) sb += lens->sb_list[k]->surface_brightness(center_pt[subcell_index][0],center_pt[subcell_index][1]);
												else {
													subpixel_xlength = pixel_xlength/sqrt(nsubpix);
													subpixel_ylength = pixel_ylength/sqrt(nsubpix);
													corner1[0] = center_pt[subcell_index][0] - subpixel_xlength/2;
													corner1[1] = center_pt[subcell_index][1] - subpixel_ylength/2;
													corner2[0] = center_pt[subcell_index][0] + subpixel_xlength/2;
													corner2[1] = center_pt[subcell_index][1] - subpixel_ylength/2;
													corner3[0] = center_pt[subcell_index][0] - subpixel_xlength/2;
													corner3[1] = center_pt[subcell_index][1] + subpixel_ylength/2;
													corner4[0] = center_pt[subcell_index][0] + subpixel_xlength/2;
													corner4[1] = center_pt[subcell_index][1] + subpixel_ylength/2;
													sb += lens->sb_list[k]->surface_brightness_zoom(center_pt[subcell_index],corner1,corner2,corner3,corner4);
												}
											}
										}
									}
								}
								surface_brightness[i][j] += sb / nsubpix;
							}
						}
					}
				}
			} else {
				for (j=0; j < y_N; j++) {
					for (i=0; i < x_N; i++) {
						//surface_brightness[i][j] = 0;
						if ((fit_to_data == NULL) or (fit_to_data[i][j])) {
							if (!foreground_only) {
								if (source_fit_mode==Delaunay_Source) surface_brightness[i][j] = delaunay_srcgrid->find_lensed_surface_brightness(center_sourcepts[i][j],i,j,0);
								else {
									surface_brightness[i][j] = source_pixel_grid->find_lensed_surface_brightness_interpolate(center_sourcepts[i][j],0);
								}
							}
						}
						if ((at_least_one_foreground_src) and (!lensed_sources_only)) {
							for (int k=0; k < lens->n_sb; k++) {
								if (!lens->sb_list[k]->is_lensed) {
									if (!lens->sb_list[k]->zoom_subgridding) {
										surface_brightness[i][j] += lens->sb_list[k]->surface_brightness(center_pts[i][j][0],center_pts[i][j][1]);
										//double meansb = lens->sb_list[k]->surface_brightness(center_pts[i][j][0],center_pts[i][j][1]);
										//cout << "ADDING SB " << meansb << endl;
									}
									else surface_brightness[i][j] += lens->sb_list[k]->surface_brightness_zoom(center_pts[i][j],corner_pts[i][j],corner_pts[i+1][j],corner_pts[i][j+1],corner_pts[i+1][j+1]);
								}
							}
						}
					}

				}
			}
		}
	}

	// Now we deal with lensed and unlensed source objects, if they exist
	bool at_least_one_lensed_src = false;
	for (int k=0; k < lens->n_sb; k++) {
		if (lens->sb_list[k]->is_lensed) {
			at_least_one_lensed_src = true;
		}
	}
	if (lens->split_imgpixels) {
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			double sb;
			lensvector corner1, corner2, corner3, corner4;
			double subpixel_xlength, subpixel_ylength;

			int nsubpix,subcell_index;
			lensvector *center_srcpt, *center_pt;
			#pragma omp for private(i,j,sb,subcell_index,nsubpix,subpixel_xlength,subpixel_ylength,center_pt,center_srcpt,corner1,corner2,corner3,corner4) schedule(dynamic)
			for (j=0; j < y_N; j++) {
				for (i=0; i < x_N; i++) {
					//surface_brightness[i][j] = 0;
					if ((fit_to_data == NULL) or (fit_to_data[i][j])) {
						sb=0;
						nsubpix = INTSQR(nsplits[i][j]);
						center_srcpt = subpixel_center_sourcepts[i][j];
						center_pt = subpixel_center_pts[i][j];
						for (subcell_index=0; subcell_index < nsubpix; subcell_index++) {
							for (int k=0; k < lens->n_sb; k++) {
								if (lens->sb_list[k]->is_lensed) {
									if (!foreground_only) {
										sb += lens->sb_list[k]->surface_brightness(center_srcpt[subcell_index][0],center_srcpt[subcell_index][1]);
									}
								}
								else if (!lensed_sources_only) {
									if (!lens->sb_list[k]->zoom_subgridding) sb += lens->sb_list[k]->surface_brightness(center_pt[subcell_index][0],center_pt[subcell_index][1]);
									else {
										subpixel_xlength = pixel_xlength/sqrt(nsubpix);
										subpixel_ylength = pixel_ylength/sqrt(nsubpix);
										corner1[0] = center_pt[subcell_index][0] - subpixel_xlength/2;
										corner1[1] = center_pt[subcell_index][1] - subpixel_ylength/2;
										corner2[0] = center_pt[subcell_index][0] + subpixel_xlength/2;
										corner2[1] = center_pt[subcell_index][1] - subpixel_ylength/2;
										corner3[0] = center_pt[subcell_index][0] - subpixel_xlength/2;
										corner3[1] = center_pt[subcell_index][1] + subpixel_ylength/2;
										corner4[0] = center_pt[subcell_index][0] + subpixel_xlength/2;
										corner4[1] = center_pt[subcell_index][1] + subpixel_ylength/2;
										sb += lens->sb_list[k]->surface_brightness_zoom(center_pt[subcell_index],corner1,corner2,corner3,corner4);
									}
								}
							}
						}
						surface_brightness[i][j] += sb / nsubpix;
					}
				}
			}
		}

		/*
		// Testing to see which pixels might need to be split, and what the criterion should be for splitting
		double sb2;
		int ncrit=0, nsp=0, nweird=0;
		for (j=0; j < y_N; j++) {
			for (i=0; i < x_N; i++) {
				sb2 = 0;
				for (int k=0; k < lens->n_sb; k++) {
					if (lens->sb_list[k]->is_lensed) {
						sb2 += lens->sb_list[k]->surface_brightness(center_sourcepts[i][j][0],center_sourcepts[i][j][1]);
					}
				}
				double difx, dify;
				if ((i > 0) and (i < x_N-1)) difx = dmax(abs(surface_brightness[i+1][j]-surface_brightness[i][j]),abs(surface_brightness[i][j]-surface_brightness[i-1][j]));
				else difx=0;
				if ((j > 0) and (j < y_N-1)) dify = dmax(abs(surface_brightness[i][j+1]-surface_brightness[i][j]),abs(surface_brightness[i][j]-surface_brightness[i][j-1]));
				else dify=0;
				if (lens->image_pixel_data->in_mask[i][j]) {
					if (abs(surface_brightness[i][j]-sb2) > (0.5*lens->data_pixel_noise)) {
						double mag = pixel_area / (source_plane_triangle1_area[i][j] + source_plane_triangle2_area[i][j]);
						cout << "MUST SPLIT! " << center_pts[i][j][0] << " " << center_pts[i][j][1] << " " << surface_brightness[i][j] << " " << sb2 << " mag=" << mag << " dif: " << difx << " " << dify;
						if ((mag < 0.1) and (lens->image_pixel_data->surface_brightness[i][j] > 0.5)) cout << " (LOWMAG) ";
						if ((mag < 10) or (lens->image_pixel_data->surface_brightness[i][j] < 0.5)) { cout << " (WEIRD)"; nweird++; }
						else nsp++;
						cout << endl;
					} else {
						double mag = pixel_area / (source_plane_triangle1_area[i][j] + source_plane_triangle2_area[i][j]);
						if ((mag > 10) and (lens->image_pixel_data->surface_brightness[i][j] > 0.5)) {
							//cout << "HI MAG! mag=" << mag << " " <<  surface_brightness[i][j] << " dif: " << difx << " " << dify << endl;
							ncrit++;
						} else if ((mag < 0.1) and (lens->image_pixel_data->surface_brightness[i][j] > 0.5)) {
							cout << "LOW MAG! mag=" << mag << " " <<  lens->image_pixel_data->surface_brightness[i][j] << " dif: " << difx << " " << dify << endl;
						}
					}
				}

			}
		}
		*/
	} else {
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			#pragma omp for private(i,j) schedule(dynamic)
			for (j=0; j < y_N; j++) {
				for (i=0; i < x_N; i++) {
					for (int k=0; k < lens->n_sb; k++) {
						if (lens->sb_list[k]->is_lensed) {
							if (!foreground_only) {
								if (!lens->sb_list[k]->zoom_subgridding) surface_brightness[i][j] += lens->sb_list[k]->surface_brightness(center_sourcepts[i][j][0],center_sourcepts[i][j][1]);
								else {
									surface_brightness[i][j] += lens->sb_list[k]->surface_brightness_zoom(center_sourcepts[i][j],corner_sourcepts[i][j],corner_sourcepts[i+1][j],corner_sourcepts[i][j+1],corner_sourcepts[i+1][j+1]);
								}
							}
						}
						else if (!lensed_sources_only) {
							if (!lens->sb_list[k]->zoom_subgridding) {
								surface_brightness[i][j] += lens->sb_list[k]->surface_brightness(center_pts[i][j][0],center_pts[i][j][1]);
							}
							else {
								surface_brightness[i][j] += lens->sb_list[k]->surface_brightness_zoom(center_pts[i][j],corner_pts[i][j],corner_pts[i+1][j],corner_pts[i][j+1],corner_pts[i+1][j+1]);

							}
						}
					}
				}
			}
		}
	}
#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for ray-tracing image surface brightness values: " << wtime << endl;
	}
#endif
}

void ImagePixelGrid::find_image_points(const double src_x, const double src_y, vector<image>& imgs, const bool use_overlap_in, const bool is_lensed, const bool verbal)
{
	imgs.resize(0);
	if (!is_lensed) {
		image imgpt;
		imgpt.pos[0] = src_x;
		imgpt.pos[1] = src_y;
		imgpt.mag = 1.0;
		imgpt.td = 0;
		imgs.push_back(imgpt);
		return;
	}
	lens->record_singular_points(imggrid_zfactors);
	static const int max_nimgs = 50;
	lens->source[0] = src_x;
	lens->source[1] = src_y;
	int i,j,npix,cell_i,cell_j,n_candidates = 0;
	SourcePixelGrid* cellptr;
	bool use_overlap = use_overlap_in;
	if (use_overlap) {
		int srcgrid_nx = source_pixel_grid->u_N;
		int srcgrid_ny = source_pixel_grid->w_N;
		double xmin, ymin, xmax, ymax;
		xmin = source_pixel_grid->cell[0][0]->corner_pt[0][0];
		ymin = source_pixel_grid->cell[0][0]->corner_pt[0][1];
		xmax = source_pixel_grid->cell[srcgrid_nx-1][srcgrid_ny-1]->corner_pt[3][0];
		ymax = source_pixel_grid->cell[srcgrid_nx-1][srcgrid_ny-1]->corner_pt[3][1];
		if ((src_x < xmin) or (src_y < ymin) or (src_x > xmax) or (src_y > ymax)) use_overlap = false;
		else {
			cell_i = (int) (srcgrid_nx * ((src_x - xmin) / (xmax - xmin)));
			cell_j = (int) (srcgrid_ny * ((src_y - ymin) / (ymax - ymin)));
			cellptr = source_pixel_grid->cell[cell_i][cell_j];
			npix = cellptr->overlap_pixel_n.size();
		}
	}
	if (!use_overlap) {
		npix = ntot_cells;
	}
	//cout << "The source cell containing this source point has " << npix << " overlapping image pixels" << endl;
	int n,imgpt_i,img_i,img_j;
#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	//cout << "SRC: " << src_x << " " << src_y << endl;
	enum InsideTri { None, Lower, Upper } inside_tri; // inside_tri = 0 if not inside; 1 if inside lower triangle; 2 if inside upper triangle
	struct imgpt_info {
		bool confirmed;
		lensvector pos;
	};
	imgpt_info image_candidates[max_nimgs]; // if there are more than 20 images, we have a truly demonic lens on our hands
	lensvector d1,d2,d3;
	double product1,product2,product3;
	// No need to parallelize this part--it is very, very fast
	//cout << "NPIX: " << npix << endl;
	lensvector *vertex1,*vertex2,*vertex3,*vertex4,*vertex5;
	lensvector *vertex1_srcplane,*vertex2_srcplane,*vertex3_srcplane,*vertex4_srcplane,*vertex5_srcplane;
	int *twist_type;
	for (i=0; i < npix; i++) {
		if (use_overlap) n = cellptr->overlap_pixel_n[i];
		else n = i;
		img_j = masked_pixels_j[n];
		img_i = masked_pixels_i[n];
		twist_type = &twist_status[img_i][img_j];
		inside_tri = None;

		if ((*twist_type)==0) {
			vertex1 = &corner_pts[img_i][img_j];
			vertex2 = &corner_pts[img_i][img_j+1];
			vertex3 = &corner_pts[img_i+1][img_j];
			vertex4 = &corner_pts[img_i][img_j+1];
			vertex5 = &corner_pts[img_i+1][img_j+1];
			vertex1_srcplane = &corner_sourcepts[img_i][img_j];
			vertex2_srcplane = &corner_sourcepts[img_i][img_j+1];
			vertex3_srcplane = &corner_sourcepts[img_i+1][img_j];
			vertex4_srcplane = &corner_sourcepts[img_i][img_j+1];
			vertex5_srcplane = &corner_sourcepts[img_i+1][img_j+1];
		} else if ((*twist_type)==1) {
			vertex1 = &corner_pts[img_i][img_j];
			vertex2 = &corner_pts[img_i+1][img_j];
			vertex3 = &center_pts[img_i][img_j]; // NOTE, the center point will probably not ray-trace exactly to the "twist point" where the sides cross in the source plane, but hopefully it's not too far off
			vertex4 = &corner_pts[img_i][img_j+1];
			vertex5 = &corner_pts[img_i+1][img_j+1];
			vertex1_srcplane = &corner_sourcepts[img_i][img_j];
			vertex2_srcplane = &corner_sourcepts[img_i+1][img_j];
			vertex3_srcplane = &twist_pts[img_i][img_j];
			//lensvector srcpt;
			//lens->find_sourcept((*vertex3),srcpt,0,imggrid_zfactors,imggrid_betafactors);
			//cout << "TWISTPT VS CENTER_SRCPT: " << (*vertex3_srcplane)[0] << " " << (*vertex3_srcplane)[1] << " vs " << srcpt[0] << " " << srcpt[1] << endl;
			vertex4_srcplane = &corner_sourcepts[img_i][img_j+1];
			vertex5_srcplane = &corner_sourcepts[img_i+1][img_j+1];
		} else if ((*twist_type)==2) {
			vertex1 = &corner_pts[img_i][img_j];
			vertex2 = &corner_pts[img_i][img_j+1];
			vertex3 = &center_pts[img_i][img_j]; // NOTE, the center point will probably not ray-trace exactly to the "twist point" where the sides cross in the source plane, but hopefully it's not too far off
			vertex4 = &corner_pts[img_i+1][img_j];
			vertex5 = &corner_pts[img_i+1][img_j+1];
			vertex1_srcplane = &corner_sourcepts[img_i][img_j];
			vertex2_srcplane = &corner_sourcepts[img_i][img_j+1];
			vertex3_srcplane = &twist_pts[img_i][img_j];
			//lensvector srcpt;
			//lens->find_sourcept((*vertex3),srcpt,0,imggrid_zfactors,imggrid_betafactors);
			//cout << "TWISTPT VS CENTER_SRCPT: " << (*vertex3_srcplane)[0] << " " << (*vertex3_srcplane)[1] << " vs " << srcpt[0] << " " << srcpt[1] << endl;
			vertex4_srcplane = &corner_sourcepts[img_i+1][img_j];
			vertex5_srcplane = &corner_sourcepts[img_i+1][img_j+1];
		}

		d1[0] = src_x - (*vertex1_srcplane)[0];
		d1[1] = src_y - (*vertex1_srcplane)[1];
		d2[0] = src_x - (*vertex2_srcplane)[0];
		d2[1] = src_y - (*vertex2_srcplane)[1];
		d3[0] = src_x - (*vertex3_srcplane)[0];
		d3[1] = src_y - (*vertex3_srcplane)[1];
		product1 = d1[0]*d2[1] - d1[1]*d2[0];
		product2 = d3[0]*d1[1] - d3[1]*d1[0];
		product3 = d2[0]*d3[1] - d2[1]*d3[0];
		if ((product1 > 0) and (product2 > 0) and (product3 > 0)) inside_tri = Lower;
		else if ((product1 < 0) and (product2 < 0) and (product3 < 0)) inside_tri = Lower;
		else {
			//if (product1 > 0) {
				//if ((abs(product2)==0) and (product3 > 0)) inside_tri = Lower;
				//if ((product2 > 0) and (abs(product3)==0)) inside_tri = Lower;
			//} else if (product1 < 0) {
				//if ((abs(product2)==0) and (product3 < 0)) inside_tri = Lower;
				//if ((product2 < 0) and (abs(product3)==0)) inside_tri = Lower;
			//}
			if (inside_tri==None) {
				d1[0] = src_x - (*vertex5_srcplane)[0];
				d1[1] = src_y - (*vertex5_srcplane)[1];
				d2[0] = src_x - (*vertex4_srcplane)[0];
				d2[1] = src_y - (*vertex4_srcplane)[1];
				product1 = d1[0]*d2[1] - d1[1]*d2[0];
				product2 = d3[0]*d1[1] - d3[1]*d1[0];
				product3 = d2[0]*d3[1] - d2[1]*d3[0];
				if ((product1 > 0) and (product2 > 0) and (product3 > 0)) inside_tri = Upper;
				if ((product1 < 0) and (product2 < 0) and (product3 < 0)) inside_tri = Upper;
			}
		}
		double kap;
		lensvector side1, side2;
		lensvector side1_srcplane, side2_srcplane;
		if ((inside_tri != None) and (*twist_type!=0)) warn(lens->newton_warnings,"possible image identified in cell with nonzero twist status (%g,%g); status %i",center_pts[img_i][img_j][0],center_pts[img_i][img_j][1],*twist_type);
		if (inside_tri != None) {
			//cout << "i=" << img_i << " j=" << img_j << " twiststat=" << *twist_type << endl;
			imgpt_i = n_candidates++;
			if (n_candidates > max_nimgs) die("exceeded max number of images in ImagePixelGrid::find_image_points");
			//pixels_with_imgpts[imgpt_i].img_i = img_i;
			//pixels_with_imgpts[imgpt_i].img_j = img_j;
			//pixels_with_imgpts[imgpt_i].upper_tri = (inside_tri==Upper) ? true : false;
			image_candidates[imgpt_i].confirmed = false;
			if (inside_tri==Lower) {
				image_candidates[imgpt_i].pos[0] = ((*vertex1)[0] + (*vertex2)[0] + (*vertex3)[0])/3;
				image_candidates[imgpt_i].pos[1] = ((*vertex1)[1] + (*vertex2)[1] + (*vertex3)[1])/3;
				// For now, just don't bother with central image points when using a pixel image--it only causes trouble in the form of duplicate images
				//if (!lens->include_central_image) {
				if (*twist_type==0) { // central images are unlikely to be highly magnified near a critical curve, so twisting is unlikely in that case
					kap = lens->kappa(image_candidates[imgpt_i].pos,imggrid_zfactors,imggrid_betafactors);
					side1 = corner_pts[img_i][img_j+1] - corner_pts[img_i][img_j];
					side2 = corner_pts[img_i][img_j] - corner_pts[img_i+1][img_j+1];
					side1_srcplane = corner_sourcepts[img_i][img_j+1] - corner_sourcepts[img_i][img_j];
					side2_srcplane = corner_sourcepts[img_i][img_j] - corner_sourcepts[img_i+1][img_j+1];
					product1 = side1 ^ side2;
					product2 = side1_srcplane ^ side2_srcplane;
					//if (product1*product2 > 0) cout << "Parity > 0" << endl;
				}

				//cout << "#Lower triangle: " << endl;
				//cout << corner_sourcepts[img_i][img_j][0] << " " << corner_sourcepts[img_i][img_j][1] << endl;
				//cout << corner_sourcepts[img_i][img_j+1][0] << " " << corner_sourcepts[img_i][img_j+1][1] << endl;
				//cout << corner_sourcepts[img_i+1][img_j][0] << " " << corner_sourcepts[img_i+1][img_j][1] << endl;
				//cout << corner_sourcepts[img_i][img_j][0] << " " << corner_sourcepts[img_i][img_j][1] << endl;
				//cout << endl;
			} else {
				image_candidates[imgpt_i].pos[0] = ((*vertex3)[0] + (*vertex4)[0] + (*vertex5)[0])/3;
				image_candidates[imgpt_i].pos[1] = ((*vertex3)[1] + (*vertex4)[1] + (*vertex5)[1])/3;
				//if (!lens->include_central_image) {
				if (*twist_type==0) { // central images are unlikely to be highly magnified near a critical curve, so twisting is unlikely in that case
					kap = lens->kappa(image_candidates[imgpt_i].pos,imggrid_zfactors,imggrid_betafactors);
					//if (kap > 1) cout << "CENTRAL IMAGE? candidate pos=" << image_candidates[imgpt_i].pos[0] << "," << image_candidates[imgpt_i].pos[1] << endl;
					side1 = corner_pts[img_i+1][img_j+1] - corner_pts[img_i+1][img_j];
					side2 = corner_pts[img_i][img_j+1] - corner_pts[img_i+1][img_j+1];
					side1_srcplane = corner_sourcepts[img_i+1][img_j+1] - corner_sourcepts[img_i+1][img_j];
					side2_srcplane = corner_sourcepts[img_i][img_j+1] - corner_sourcepts[img_i+1][img_j+1];
					product1 = side1 ^ side2;
					product2 = side1_srcplane ^ side2_srcplane;
					//if (product1*product2 > 0) cout << "Parity > 0" << endl;
				}


				//cout << "#Upper triangle: " << endl;
				//cout << corner_sourcepts[img_i+1][img_j+1][0] << " " << corner_sourcepts[img_i+1][img_j+1][1] << endl;
				//cout << corner_sourcepts[img_i][img_j+1][0] << " " << corner_sourcepts[img_i][img_j+1][1] << endl;
				//cout << corner_sourcepts[img_i+1][img_j][0] << " " << corner_sourcepts[img_i+1][img_j][1] << endl;
				//cout << corner_sourcepts[img_i+1][img_j+1][0] << " " << corner_sourcepts[img_i+1][img_j+1][1] << endl;
			}
			//if ((!lens->include_central_image) and (kap > 1) and (product1*product2 > 0)) n_candidates--;
			if ((*twist_type==0) and (kap > 1) and (product1*product2 > 0)) n_candidates--; // exclude central image candidate by default
		}

		//cout << "Pixel " << img_i << "," << img_j << endl;
	}
	image_pos_accuracy = Grid::image_pos_accuracy;
	//image_pos_accuracy = 0.05*lens->data_pixel_size;
	//if (image_pos_accuracy < 0) image_pos_accuracy = 1e-3; // in case the data pixel size has not been set
	if ((lens->mpi_id==0) and (verbal)) cout << "Found " << n_candidates << " candidate images" << endl;
	//for (i=0; i < n_candidates; i++) {
		//cout << "candidate " << i << ": " << image_candidates[i].pos[0] << " " << image_candidates[i].pos[1] << endl;
	//}
	//cout << endl;
	#pragma omp parallel
	{
		int thread;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
		image imgpt;
		double mag;
		#pragma omp for private(i) schedule(static)
		for (i=0; i < n_candidates; i++) {
			//cout << "Trying candidate " << i << ": " << image_candidates[i].pos[0] << " " << image_candidates[i].pos[1] << endl;
			if (run_newton(image_candidates[i].pos,mag,thread)==true) {
				imgpt.pos = image_candidates[i].pos;
				//imgpt.mag = lens->magnification(image_candidates[i].pos,0,imggrid_zfactors,imggrid_betafactors);
				imgpt.mag = mag;
				imgpt.flux = -1e30;
				//cout << "FOUND IMAGE AT: " << imgpt.pos[0] << " " << imgpt.pos[1] << endl;
				//if ((lens->mpi_id==0) and (verbal)) cout << "FOUND IMAGE AT: " << imgpt.pos[0] << " " << imgpt.pos[1] << endl;
				if (lens->include_time_delays) {
					double potential = lens->potential(imgpt.pos,imggrid_zfactors,imggrid_betafactors);
					imgpt.td = 0.5*(SQR(imgpt.pos[0]-lens->source[0])+SQR(imgpt.pos[1]-lens->source[1])) - potential; // the dimensionless version; it will be converted to days by the QLens class
				} else {
					imgpt.td = 0;
				}
				imgpt.parity = sign(imgpt.mag);
				#pragma omp critical
				{
					imgs.push_back(imgpt);
				}
			}
		}
	}
	bool redundancy;
	vector<image>::iterator it, it2;
	double sep, pixel_size;
	pixel_size = dmax(pixel_xlength,pixel_ylength);
	//int oldsize = imgs.size();
	if (imgs.size() > 1) {
		for (it = imgs.begin()+1; it != imgs.end(); it++) {
			redundancy = false;
			for (it2 = imgs.begin(); it2 != it; it2++) {
				sep = sqrt(SQR(it->pos[0] - it2->pos[0]) + SQR(it->pos[1] - it2->pos[1]));
				if (sep < pixel_size)
				{
					redundancy = true;
					warn(lens->newton_warnings,"rejecting probable duplicate image (imgsep=%g,threshold=%g): src (%g,%g), image (%g,%g), mag %g",sep,pixel_size,lens->source[0],lens->source[1],it->pos[0],it->pos[1],it->mag);
					break;
				}
			}
			if (redundancy) {
				if (it == imgs.end()-1) {
					imgs.pop_back();
					break;
				} else {
					imgs.erase(it);
					it--;
				}
			}
		}
	}
	if (imgs.size() > 4) warn(lens->newton_warnings,"more than four images after trimming redundancies");

	if ((lens->mpi_id==0) and (verbal)) {
		cout << "# images found: " << imgs.size() << endl;
		for (i=0; i < imgs.size(); i++) {
			cout << imgs[i].pos[0] << " " << imgs[i].pos[1] << " " << imgs[i].mag << endl;
		}
	}
#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for point image finding: " << wtime << endl;
	}
#endif
}

void ImagePixelGrid::generate_point_images(const vector<image>& imgs, double *ptimage_surface_brightness, const bool use_img_fluxes, const double srcflux, const int img_num)
{
	int nx_half, ny_half;
	double normfac, sigx, sigy;
	int nsplit = lens->psf_ptsrc_nsplit;
	int nsubpix = nsplit*nsplit;
	if (lens->use_input_psf_matrix) {
		if (lens->psf_matrix == NULL) return;
		nx_half = lens->psf_npixels_x/2;
		ny_half = lens->psf_npixels_y/2;
	} else {
		double sigma_fraction = sqrt(-2*log(lens->psf_ptsrc_threshold));
		double nx_half_dec, ny_half_dec;
		sigx = lens->psf_width_x;
		sigy = lens->psf_width_y;
		nx_half_dec = sigma_fraction*sigx/pixel_xlength;
		ny_half_dec = sigma_fraction*sigy/pixel_ylength;
		//cout << "sigma_frac=" << sigma_fraction << endl;
		//cout << "nxhalfd=" << nx_half_dec << " nyhalfd=" << ny_half_dec << endl;
		nx_half = ((int) nx_half_dec);
		ny_half = ((int) ny_half_dec);
		if ((nx_half_dec - nx_half) > 0.5) nx_half++;
		if ((ny_half_dec - ny_half) > 0.5) ny_half++;
		normfac = 1.0/(M_2PI*sigx*sigy);
	}
	//cout << "nxhalf=" << nx_half << " nyhalf=" << ny_half << endl;
	//int nx = 2*nx_half+1;
	//int ny = 2*ny_half+1;
	//cout << "normfac=" << normfac << endl;

	int i,j,ii,jj,img_i;
	int i_center, j_center, imin, imax, jmin, jmax;
	//cout << "nsplit=" << nsplit << " nsubpix=" << nsubpix << endl;
	double sb, fluxfac, x, y, x0, y0, u0, w0;
#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	int img_i0, img_if;
	if (img_num==-1) {
		img_i0 = 0;
		img_if = imgs.size();
	} else {
		if (img_num >= imgs.size()) die("img_num does not exist; cannot generate point image");
		img_i0 = img_num;
		img_if = img_num+1;
	}
	for (int indx=0; indx < lens->image_npixels; indx++) ptimage_surface_brightness[indx] = 0;
	for (img_i=img_i0; img_i < img_if; img_i++) {
		if ((use_img_fluxes) and (imgs[img_i].flux != -1e30)) {
			//cout << "USING IMAGE FLUX: " << imgs[img_i].flux << endl;
			fluxfac = imgs[img_i].flux;
		} else {
			//cout << "NOT USING IMAGE FLUX! " << imgs[img_i].flux << " srcflux=" << srcflux << endl;
			if (srcflux >= 0) fluxfac = srcflux*abs(imgs[img_i].mag);
			else fluxfac = 1.0; // if srcflux < 0, then normalize to 1 (this will be multiplied by a flux amplitude afterwards)
		}
		//cout << "flux=" << flux << endl;
		x0 = imgs[img_i].pos[0];
		y0 = imgs[img_i].pos[1];
		i_center = (x0 - xmin)/pixel_xlength;
		j_center = (y0 - ymin)/pixel_ylength;
		//cout << "icenter=" << i_center << " jcenter=" << j_center << endl;
		if ((i_center < 0) or (i_center >= x_N) or (j_center < 0) or (j_center >= y_N)) {
			warn("image point lies outside image pixel grid");
			continue;
		}
		imin = i_center - nx_half;
		imax = i_center + nx_half;
		jmin = j_center - ny_half;
		jmax = j_center + ny_half;
		if (imin < 0) imin = 0;
		if (imax >= x_N) imax = x_N-1;
		if (jmin < 0) jmin = 0;
		if (jmax >= y_N) jmax = y_N-1;
		//cout << "imin=" << imin << " imax=" << imax << " jmin=" << jmin << " jmax=" << jmax << endl;
		//cout << "xmin=" << corner_pts[imin][jmin][0] << " xmax=" << corner_pts[imax][jmin][0] << endl;
		//cout << "ymin=" << corner_pts[imin][jmin][1] << " ymax=" << corner_pts[imin][jmax][1] << endl;
		//cout << "x0(pixel_center)=" << center_pts[i_center][j_center][0] << "y0(pixel_center)=" << center_pts[i_center][j_center][1] << endl;
		//double norm = 0;
		//for (i=imin; i <= imax; i++) {
			//for (j=jmin; j <= jmax; j++) {
				//sb += flux*exp(-(SQR((center_pts[i][j][0]-x0)/sigx) + SQR((center_pts[i][j][1]-y0)/sigy))/2);
				//surface_brightness[i][j] += sb;
				//norm += sb;
			//}
		//}

		//double tot=0;
		for (i=imin; i <= imax; i++) {
			for (j=jmin; j <= jmax; j++) {
				sb=0;
				for (ii=0; ii < nsplit; ii++) {
					u0 = ((double) (1+2*ii))/(2*nsplit);
					x = u0*corner_pts[i][j][0] + (1-u0)*corner_pts[i+1][j][0];
					for (jj=0; jj < nsplit; jj++) {
						w0 = ((double) (1+2*jj))/(2*nsplit);
						y = w0*corner_pts[i][j][1] + (1-w0)*corner_pts[i][j+1][1];
						if (lens->use_input_psf_matrix) {
							sb += fluxfac*lens->interpolate_PSF_matrix(x-x0,y-y0)/(pixel_xlength*pixel_ylength);
						} else {
							sb += fluxfac*normfac*exp(-(SQR((x-x0)/sigx) + SQR((y-y0)/sigy))/2);
						}
					}
				}
				sb /= nsubpix;
				//cout << "sb=" << sb << endl;
				if ((fit_to_data==NULL) or (fit_to_data[i][j])) {
					ptimage_surface_brightness[pixel_index[i][j]] += sb;
					//tot += sb;
				}
			}
		}
		//sigx = lens->psf_width_x;
		//sigy = lens->psf_width_y;
		//normfac = 1.0/(M_2PI*sigx*sigy);
		//double sbcheck0 = lens->psf_matrix[nx_half][ny_half]/(pixel_xlength*pixel_ylength);
		//double sbcheck = lens->interpolate_PSF_matrix(0,0)/(pixel_xlength*pixel_ylength);
		//double sbcheck2 = normfac;
		////cout << "normfac_inv: " << (1.0/normfac) << endl;
		//cout << "SBCHECKS: " << nx_half << " " << ny_half << " " << sbcheck0 << " " << sbcheck << " " << sbcheck2 << endl;


		//cout << "TOT=" << tot << endl;
		//cout << "Added point image" << endl;
		//cout << "area = " << (nx*ny*pixel_xlength*pixel_ylength) << endl;
	}
#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for adding point images: " << wtime << endl;
	}
#endif
}

void ImagePixelGrid::add_point_images(double *ptimage_surface_brightness, const int npix)
{
	int i,j,indx;
	for (indx=0; indx < npix; indx++) {
		i = lens->active_image_pixel_i[indx];
		j = lens->active_image_pixel_j[indx];
		surface_brightness[i][j] += ptimage_surface_brightness[indx];
	}
}

void ImagePixelGrid::generate_and_add_point_images(const vector<image>& imgs, const bool include_imgfluxes, const double srcflux)
{
	double *ptimgs = new double[lens->image_npixels];
	generate_point_images(imgs,ptimgs,include_imgfluxes,srcflux);
	add_point_images(ptimgs,lens->image_npixels);
	delete[] ptimgs;
}	

// 2-d Newton's Method w/ backtracking routines
// These functions are redundant with the same ones in the Grid class, and I *HATE* redundancies like this. But for now, it's
// easiest to just copy it over. Later you can put NewtonsMethod in a separate class and have it inherited by both Grid and ImagePixelGrid.

const int ImagePixelGrid::max_iterations = 200;
const int ImagePixelGrid::max_step_length = 100;

inline void ImagePixelGrid::SolveLinearEqs(lensmatrix& a, lensvector& b)
{
	double det, temp;
	det = determinant(a);
	temp = (-a[1][0]*b[1]+a[1][1]*b[0]) / det;
	b[1] = (-a[0][1]*b[0]+a[0][0]*b[1]) / det;
	b[0] = temp;
}

inline double ImagePixelGrid::max_component(const lensvector& x) { return dmax(fabs(x[0]),fabs(x[1])); }

bool ImagePixelGrid::run_newton(lensvector& xroot, double& mag, const int& thread)
{
	lensvector xroot_initial = xroot;
	if ((xroot[0]==0) and (xroot[1]==0)) { xroot[0] = xroot[1] = 5e-1*lens->cc_rmin; }	// Avoiding singularity at center
	if (NewtonsMethod(xroot, newton_check[thread], thread)==false) {
		warn(lens->newton_warnings,"Newton's method failed for source (%g,%g), initial point (%g,%g)",lens->source[0],lens->source[1],xroot_initial[0],xroot_initial[1]);
		return false;
	}
	//if (lens->reject_images_found_outside_cell) {
		//if (test_if_inside_cell(xroot,thread)==false) {
			////warn(lens->warnings,"Rejecting image found outside cell for source (%g,%g), level %i, cell center (%g,%g)",lens->source[0],lens->source[1],level,center_imgplane[0],center_imgplane[1],xroot[0],xroot[1]);
			//return false;
		//}
	//}

	lensvector lens_eq_f;
	lens->lens_equation(xroot,lens_eq_f,thread,imggrid_zfactors,imggrid_betafactors);
	//double lenseq_mag = sqrt(SQR(lens_eq_f[0]) + SQR(lens_eq_f[1]));
	//double tryacc = image_pos_accuracy / sqrt(abs(lens->magnification(xroot,thread,zfactor)));
	//cout << lenseq_mag << " " << tryacc << " " << sqrt(abs(lens->magnification(xroot,thread,zfactor))) << endl;
	if (newton_check[thread]==true) { warn(lens->newton_warnings, "false image--converged to local minimum"); return false; }
	if (lens->n_singular_points > 0) {
		//cout << "singular point: " << lens->singular_pts[0][0] << " " << lens->singular_pts[0][1] << endl;
		double singular_pt_accuracy = 2*image_pos_accuracy;
		for (int i=0; i < lens->n_singular_points; i++) {
			if ((abs(xroot[0]-lens->singular_pts[i][0]) < singular_pt_accuracy) and (abs(xroot[1]-lens->singular_pts[i][1]) < singular_pt_accuracy)) {
				warn(lens->newton_warnings,"Newton's method converged to singular point (%g,%g) for source (%g,%g)",lens->singular_pts[i][0],lens->singular_pts[i][1],lens->source[0],lens->source[1]);
				return false;
			}
		}
	}
	if (((xroot[0]==xroot_initial[0]) and (xroot_initial[0] != 0)) and ((xroot[1]==xroot_initial[1]) and (xroot_initial[1] != 0)))
		warn(lens->newton_warnings, "Newton's method returned initial point");
	mag = lens->magnification(xroot,thread,imggrid_zfactors,imggrid_betafactors);
	if ((abs(lens_eq_f[0]) > 1000*image_pos_accuracy) and (abs(lens_eq_f[1]) > 1000*image_pos_accuracy)) {
		if (lens->newton_warnings==true) {
			warn(lens->newton_warnings,"Newton's method found probable false root (%g,%g) (within 1000*accuracy) for source (%g,%g), cell center (%g,%g), mag %g",xroot[0],xroot[1],lens->source[0],lens->source[1],xroot_initial[0],xroot_initial[1],xroot[0],xroot[1],mag);
		}
		return false;
	}
	if ((abs(mag) > lens->newton_magnification_threshold) or (mag*0.0 != 0.0)) {
		if (lens->reject_himag_images) {
			if ((lens->mpi_id==0) and (lens->newton_warnings)) {
				cout << "*WARNING*: Rejecting image that exceeds imgsrch_mag_threshold (" << abs(mag) << "), src=(" << lens->source[0] << "," << lens->source[1] << "), x=(" << xroot[0] << "," << xroot[1] << ")      " << endl;
				if (lens->use_ansi_characters) {
					cout << "                                                                                                                            " << endl;
					cout << "\033[2A";
				}
			}
			return false;
		} else {
			if ((lens->mpi_id==0) and (lens->warnings)) {
				cout << "*WARNING*: Image exceeds imgsrch_mag_threshold (" << abs(mag) << "); src=(" << lens->source[0] << "," << lens->source[1] << "), x=(" << xroot[0] << "," << xroot[1] << ")        " << endl;
				if (lens->use_ansi_characters) {
					cout << "                                                                                                                            " << endl;
					cout << "\033[2A";
				}
			}
		}
	}
	if ((lens->include_central_image==false) and (mag > 0) and (lens->kappa(xroot,imggrid_zfactors,imggrid_betafactors) > 1)) return false; // discard central image if not desired

	/*
	bool status = true;
	//#pragma omp critical
	//{
			images[nfound].pos[0] = xroot[0];
			images[nfound].pos[1] = xroot[1];
			images[nfound].mag = lens->magnification(xroot,0,imggrid_zfactors,imggrid_betafactors);
			if (lens->include_time_delays) {
				double potential = lens->potential(xroot,imggrid_zfactors,imggrid_betafactors);
				images[nfound].td = 0.5*(SQR(xroot[0]-lens->source[0])+SQR(xroot[1]-lens->source[1])) - potential; // the dimensionless version; it will be converted to days by the QLens class
			} else {
				images[nfound].td = 0;
			}
			images[nfound].parity = sign(images[nfound].mag);

			if (lens->use_cc_spline) {
				bool found_pos=false, found_neg=false;
				double rroot, thetaroot, cr0, cr1;
				rroot = norm(xroot[0]-lens->grid_xcenter,xroot[1]-lens->grid_ycenter);
				thetaroot = angle(xroot[0]-lens->grid_xcenter,xroot[1]-lens->grid_ycenter);
				cr0 = lens->ccspline[0].splint(thetaroot);
				cr1 = lens->ccspline[1].splint(thetaroot);

				int expected_parity;
				if (rroot < cr0) {
					nfound_max++; expected_parity = 1;
				} else if (rroot > cr1) {
					nfound_pos++; expected_parity = 1;
				} else {
					nfound_neg++; expected_parity = -1;
				}

				if (images[nfound].parity != expected_parity)
					warn(lens->warnings, "wrong parity found for image from source (%g, %g)", lens->source[0], lens->source[1]);
				
				if ((lens->system_type==Single) and (nfound_pos >= 1)) finished_search = true;
				else
				{
					if ((lens->system_type==Double) and (nfound_pos >= 1)) found_pos = true;
					else if (((lens->system_type==Quad) or (lens->system_type==Cusp)) and (nfound_pos >= 2)) found_pos = true;

					if (((lens->system_type==Double) or (lens->system_type==Cusp)) and (nfound_neg >= 1)) found_neg = true;
					else if ((lens->system_type==Quad) and (nfound_neg >= 2)) found_neg = true;

					if ((found_pos) and (found_neg)) finished_search = true;
				}
			}

			nfound++;
		}
		*/
	//}
	return true;
}

bool ImagePixelGrid::NewtonsMethod(lensvector& x, bool &check, const int& thread)
{
	check = false;
	lensvector g, p, xold;
	lensmatrix fjac;

	lens->lens_equation(x, fvec[thread], thread, imggrid_zfactors, imggrid_betafactors);
	double f = 0.5*fvec[thread].sqrnorm();
	if (max_component(fvec[thread]) < 0.01*image_pos_accuracy)
		return true; 

	double fold, stpmax, temp, test;
	stpmax = max_step_length * dmax(x.norm(), 2.0); 
	for (int its=0; its < max_iterations; its++) {
		lens->hessian(x[0],x[1],fjac,thread,imggrid_zfactors,imggrid_betafactors);
		fjac[0][0] = -1 + fjac[0][0];
		fjac[1][1] = -1 + fjac[1][1];
		g[0] = fjac[0][0] * fvec[thread][0] + fjac[0][1]*fvec[thread][1];
		g[1] = fjac[1][0] * fvec[thread][0] + fjac[1][1]*fvec[thread][1];
		xold[0] = x[0];
		xold[1] = x[1];
		fold = f; 
		p[0] = -fvec[thread][0];
		p[1] = -fvec[thread][1];
		SolveLinearEqs(fjac, p);
		if (LineSearch(xold, fold, g, p, x, f, stpmax, check, thread)==false)
			return false;
		if ((x[0] > 1e3*lens->cc_rmax) or (x[1] > 1e3*lens->cc_rmax)) {
			warn(lens->newton_warnings, "Newton blew up!");
			return false;
		}
		/*
		lens->lens_equation(x, fvec[thread], thread, zfactor);
		double magfac = sqrt(abs(lens->magnification(x,thread,zfactor)));
		double tryacc;
		lensvector dx = x - xold;
		double dxnorm = dx.norm();
		dx[0] /= dxnorm;
		dx[1] /= dxnorm;
		lensmatrix magmat;
		lensvector bb;
		lens->sourcept_jacobian(x,bb,magmat,thread,zfactor);
		bb = magmat*dx;
		lensvector dy;
		dy[1] = -dx[0];
		dy[0] = dx[1];
		lensvector cc;
		tryacc = image_pos_accuracy * bb.norm();
		*/
		//if (max_component(fvec[thread]) < 4*tryacc) {

		// Maybe someday revisit this and see if you can make it more robust. As it is, it's
		// frustrating that image_pos_accuracy has no simple interpretation, and occasionally
		// spurious images close to critical curves do are found.
		if (max_component(fvec[thread]) < image_pos_accuracy) {
			check = false; 
			return true; 
		}
		if (check) {
			double den = dmax(f, 1.0); 
			temp = fabs(g[0]) * dmax(fabs(x[0]), 1.0)/den; 
			test = fabs(g[1]) * dmax(fabs(x[1]), 1.0)/den; 
			check = (dmax(test,temp) < image_pos_accuracy); 
			return true; 
		}
		test = (fabs(x[0] - xold[0])) / dmax(fabs(x[0]), 1.0); 
		temp = (fabs(x[1] - xold[1])) / dmax(fabs(x[1]), 1.0); 
		if (temp > test) test = temp; 
		if (test < image_pos_accuracy) return true; 
	}

	return false;
}

bool ImagePixelGrid::LineSearch(lensvector& xold, double fold, lensvector& g, lensvector& p, lensvector& x,
	double& f, double stpmax, bool &check, const int& thread)
{
	const double alpha = 1.0e-4;	// Ensures sufficient decrease in function value (see NR Ch. 9.7)

	double a, alam, alam2, alamin, b, disc, f2, rhs1, rhs2, slope, mag, temp, test, tmplam;

	check = false;
	mag = p.norm();
	if (mag > stpmax) {
		double fac = stpmax / mag;
		p[0] *= fac;
		p[1] *= fac;
	}
	slope = g[0]*p[0] + g[1]*p[1];
	if (slope >= 0.0) die("Roundoff problem during line search (g=(%g,%g), p=(%g,%g))",g[0],g[1],p[0],p[1]); 
	test = fabs(p[0]) / dmax(fabs(xold[0]), 1.0); 
	temp = fabs(p[1]) / dmax(fabs(xold[1]), 1.0); 
	alamin = image_pos_accuracy / dmax(temp,test); 
	alam = 1.0; 
	while (true)
	{
		x[0] = xold[0] + alam*p[0];
		x[1] = xold[1] + alam*p[1];
		if ((fabs(x[0]) < 1e6*lens->cc_rmax) and (fabs(x[1]) < 1e6*lens->cc_rmax))
			;
		else {
			warn(lens->newton_warnings, "Newton blew up!");
			return false;
		}
		lens->lens_equation(x, fvec[thread], thread, imggrid_zfactors, imggrid_betafactors);
		f = 0.5 * fvec[thread].sqrnorm();
		if (alam < alamin) {
			x[0] = xold[0];
			x[1] = xold[1];
			check = true; 
			return true; 
		} else if (f <= fold + alpha*alam*slope)
			return true; 
		else
		{
			if (alam == 1.0)
				tmplam = -slope / (2.0*(f-fold-slope));
			else
			{
				rhs1 = f - fold - alam*slope;
				rhs2 = f2 - fold - alam2*slope;
				a = (rhs1/(alam*alam) - rhs2/(alam2*alam2)) / (alam-alam2);
				b = (-alam2*rhs1/(alam*alam) + alam*rhs2/(alam2*alam2)) / (alam-alam2);
				if (a == 0.0) tmplam = -slope / (2.0*b);
				else
				{
					disc = b*b - 3.0*a*slope;
					if (disc < 0.0) tmplam = 0.5*alam;
					else if (b <= 0.0) tmplam = (-b + sqrt(disc)) / (3.0*a);
					else tmplam = -slope / (b + sqrt(disc));
				}
				if (tmplam > 0.5*alam)
					tmplam = 0.5*alam;
			}
		}
		alam2 = alam;
		f2 = f;
		alam = dmax(tmplam, 0.1*alam);
	}
}

double QLens::find_surface_brightness(lensvector &pt)
{
	//double xl=0.01, yl=0.01;
	//lensvector pt1,pt2,pt3,pt4;
	//pt1[0] = pt[0] - xl/2;
	//pt1[1] = pt[1] - yl/2;

	//pt2[0] = pt[0] + xl/2;
	//pt2[1] = pt[1] - yl/2;

	//pt3[0] = pt[0] - xl/2;
	//pt3[1] = pt[1] + yl/2;

	//pt4[0] = pt[0] + xl/2;
	//pt4[1] = pt[1] + yl/2;
	double sb = 0;
	lensvector srcpt;
	
	find_sourcept(pt,srcpt,0,reference_zfactors,default_zsrc_beta_factors);
	//cout << "src=" << srcpt[0] << "," << srcpt[1] << " pt=" << pt[0] << "," << pt[1] << endl;
	for (int k=0; k < n_sb; k++) {
		if (sb_list[k]->is_lensed) {
			sb += sb_list[k]->surface_brightness(srcpt[0],srcpt[1]);
			//if (!sb_list[k]->zoom_subgridding) sb += sb_list[k]->surface_brightness(srcpt[0],srcpt[1]);
			//else {
				//lensvector srcpt1,srcpt2,srcpt3,srcpt4;
				//find_sourcept(pt1,srcpt1,0,reference_zfactors,default_zsrc_beta_factors);
				//find_sourcept(pt2,srcpt2,0,reference_zfactors,default_zsrc_beta_factors);
				//find_sourcept(pt3,srcpt3,0,reference_zfactors,default_zsrc_beta_factors);
				//find_sourcept(pt4,srcpt4,0,reference_zfactors,default_zsrc_beta_factors);
				//sb += sb_list[k]->surface_brightness_zoom(pt,srcpt1,srcpt2,srcpt3,srcpt4);
			//}
		} else {
			sb += sb_list[k]->surface_brightness(pt[0],pt[1]);
			//if (!sb_list[k]->zoom_subgridding) sb += sb_list[k]->surface_brightness(pt[0],pt[1]);
			//else sb += sb_list[k]->surface_brightness_zoom(pt,pt1,pt2,pt3,pt4);
		}
		//cout << "object " << k << ": sb=" << sb << endl;
	}
	return sb;
}

ImagePixelGrid::~ImagePixelGrid()
{
	for (int i=0; i <= x_N; i++) {
		delete[] corner_pts[i];
		delete[] corner_sourcepts[i];
	}
	delete[] corner_pts;
	delete[] corner_sourcepts;
	for (int i=0; i < x_N; i++) {
		delete[] center_pts[i];
		delete[] center_sourcepts[i];
		delete[] maps_to_source_pixel[i];
		delete[] pixel_index[i];
		delete[] pixel_index_fgmask[i];
		delete[] mapped_cartesian_srcpixels[i];
		delete[] mapped_delaunay_srcpixels[i];
		delete[] surface_brightness[i];
		delete[] source_plane_triangle1_area[i];
		delete[] source_plane_triangle2_area[i];
		delete[] twist_status[i];
		delete[] twist_pts[i];
		delete[] nsplits[i];
		//for (int j=0; j < y_N; j++) {
			//delete[] subpixel_maps_to_srcpixel[i][j];
			//delete[] subpixel_center_pts[i][j];
			//delete[] subpixel_center_sourcepts[i][j];
		//}
		//delete[] subpixel_maps_to_srcpixel[i];
		//delete[] subpixel_center_pts[i];
		//delete[] subpixel_center_sourcepts[i];
	}
	delete[] center_pts;
	delete[] center_sourcepts;
	delete[] maps_to_source_pixel;
	delete[] pixel_index;
	delete[] pixel_index_fgmask;
	delete[] mapped_cartesian_srcpixels;
	delete[] mapped_delaunay_srcpixels;
	delete[] surface_brightness;
	delete[] source_plane_triangle1_area;
	delete[] source_plane_triangle2_area;
	//delete[] subpixel_maps_to_srcpixel;
	//delete[] subpixel_center_pts;
	//delete[] subpixel_center_sourcepts;
	delete[] nsplits;
	delete[] twist_status;
	delete[] twist_pts;
	if (fit_to_data != NULL) {
		for (int i=0; i < x_N; i++) delete[] fit_to_data[i];
		delete[] fit_to_data;
	}
	delete_ray_tracing_arrays();
}

/************************** Functions in class QLens that pertain to pixel mapping and inversion ****************************/

bool QLens::assign_pixel_mappings(bool verbal)
{

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int tot_npixels_count;
	if (source_fit_mode==Delaunay_Source) {
		image_pixel_grid->assign_image_mapping_flags(true);
		source_npixels = delaunay_srcgrid->assign_active_indices_and_count_source_pixels(activate_unmapped_source_pixels);
	} else {
		tot_npixels_count = source_pixel_grid->assign_indices_and_count_levels();
		if ((mpi_id==0) and (adaptive_subgrid) and (verbal==true)) cout << "Number of source cells: " << tot_npixels_count << endl;
		image_pixel_grid->assign_image_mapping_flags(false);

		source_pixel_grid->regrid = false;
		source_npixels = source_pixel_grid->assign_active_indices_and_count_source_pixels(regrid_if_unmapped_source_subpixels,activate_unmapped_source_pixels,exclude_source_pixels_beyond_fit_window);
		if (source_n_amps==0) { warn("number of source pixels cannot be zero"); return false; }
		while (source_pixel_grid->regrid) {
			if ((mpi_id==0) and (verbal==true)) cout << "Redrawing the source grid after reverse-splitting unmapped source pixels...\n";
			source_pixel_grid->regrid = false;
			source_pixel_grid->assign_all_neighbors();
			tot_npixels_count = source_pixel_grid->assign_indices_and_count_levels();
			if ((mpi_id==0) and (verbal==true)) cout << "Number of source cells after re-gridding: " << tot_npixels_count << endl;
			image_pixel_grid->assign_image_mapping_flags(false);
			//source_pixel_grid->print_indices();
			source_npixels = source_pixel_grid->assign_active_indices_and_count_source_pixels(regrid_if_unmapped_source_subpixels,activate_unmapped_source_pixels,exclude_source_pixels_beyond_fit_window);
		}
	}
	source_n_amps = source_npixels;
	if (include_imgfluxes_in_inversion) {
		for (int i=0; i < point_imgs.size(); i++) {
			source_n_amps += point_imgs[i].size(); // in this case, source amplitudes include point image amplitudes as well as pixel values
		}
	}

	image_npixels = image_pixel_grid->n_active_pixels;
	if (active_image_pixel_i != NULL) delete[] active_image_pixel_i;
	if (active_image_pixel_j != NULL) delete[] active_image_pixel_j;
	active_image_pixel_i = new int[image_npixels];
	active_image_pixel_j = new int[image_npixels];
	int i, j, image_pixel_index=0;
	for (j=0; j < image_pixel_grid->y_N; j++) {
		for (i=0; i < image_pixel_grid->x_N; i++) {
			if (image_pixel_grid->maps_to_source_pixel[i][j]) {
				active_image_pixel_i[image_pixel_index] = i;
				active_image_pixel_j[image_pixel_index] = j;
				image_pixel_grid->pixel_index[i][j] = image_pixel_index++;
			} else image_pixel_grid->pixel_index[i][j] = -1;
		}
	}
	if (image_pixel_index != image_npixels) die("Number of active pixels (%i) doesn't seem to match image_npixels (%i)",image_pixel_index,image_npixels);

	if ((verbal) and (mpi_id==0)) {
		if (source_fit_mode==Delaunay_Source) cout << "source # of pixels: " << delaunay_srcgrid->n_srcpts << ", # of active pixels: " << source_npixels << endl;
		else cout << "source # of pixels: " << source_pixel_grid->number_of_pixels << ", counted up as " << tot_npixels_count << ", # of active pixels: " << source_npixels << endl;
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for assigning pixel mappings: " << wtime << endl;
	}
#endif

	return true;
}

void QLens::assign_foreground_mappings(const bool use_data)
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	image_npixels_fgmask = 0;
	int i,j;
	for (j=0; j < image_pixel_grid->y_N; j++) {
		for (i=0; i < image_pixel_grid->x_N; i++) {
			if ((!image_pixel_data) or (!use_data) or (image_pixel_data->foreground_mask[i][j])) {
				image_npixels_fgmask++;
			}
		}
	}
	if (image_npixels_fgmask==0) die("no pixels in foreground mask");

	if (active_image_pixel_i_fgmask != NULL) delete[] active_image_pixel_i_fgmask;
	if (active_image_pixel_j_fgmask != NULL) delete[] active_image_pixel_j_fgmask;
	active_image_pixel_i_fgmask = new int[image_npixels_fgmask];
	active_image_pixel_j_fgmask = new int[image_npixels_fgmask];
	int image_pixel_index=0;
	for (j=0; j < image_pixel_grid->y_N; j++) {
		for (i=0; i < image_pixel_grid->x_N; i++) {
			if ((!image_pixel_data) or (!use_data) or (image_pixel_data->foreground_mask[i][j])) {
				active_image_pixel_i_fgmask[image_pixel_index] = i;
				active_image_pixel_j_fgmask[image_pixel_index] = j;
				//cout << "Assigining " << image_pixel_index << " to (" << i << "," << j << ")" << endl;
				//image_pixel_index++;
				image_pixel_grid->pixel_index_fgmask[i][j] = image_pixel_index++;
			} else image_pixel_grid->pixel_index_fgmask[i][j] = -1;
		}
	}
	sbprofile_surface_brightness = new double[image_npixels_fgmask];
	for (int i=0; i < image_npixels_fgmask; i++) sbprofile_surface_brightness[i] = 0;
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for assigning foreground pixel mappings: " << wtime << endl;
	}
#endif
}

void QLens::initialize_pixel_matrices(bool verbal)
{
	if (Lmatrix != NULL) die("Lmatrix already initialized");
	if (source_pixel_vector != NULL) die("source surface brightness vector already initialized");
	if (image_surface_brightness != NULL) die("image surface brightness vector already initialized");
	image_surface_brightness = new double[image_npixels];
	source_pixel_vector = new double[source_n_amps];
	point_image_surface_brightness = new double[image_npixels];

	bool delaunay = false;
	if (source_fit_mode==Delaunay_Source) delaunay = true;

	if (delaunay) {
		Lmatrix_n_elements = image_pixel_grid->count_nonzero_source_pixel_mappings_delaunay();
	} else {
		if (n_image_prior) {
			source_pixel_n_images = new double[source_n_amps];
			source_pixel_grid->fill_n_image_vector();
		}
		Lmatrix_n_elements = image_pixel_grid->count_nonzero_source_pixel_mappings_cartesian();
	}
	if ((mpi_id==0) and (verbal)) cout << "Expected Lmatrix_n_elements=" << Lmatrix_n_elements << endl << flush;
	Lmatrix_index = new int[Lmatrix_n_elements];
	image_pixel_location_Lmatrix = new int[image_npixels+1];
	Lmatrix = new double[Lmatrix_n_elements];
	if (include_imgfluxes_in_inversion) {
		int nimgs = 0;
		for (int i=0; i < point_imgs.size(); i++) nimgs += point_imgs[i].size();
		Lmatrix_transpose_ptimg_amps.input(nimgs,image_npixels);
	}

	if ((mpi_id==0) and (verbal)) cout << "Creating Lmatrix...\n";
	assign_Lmatrix(delaunay,verbal);
}

void QLens::count_shapelet_npixels()
{
	double nmax;
	source_npixels = 0;
	for (int i=0; i < n_sb; i++) {
		if (sb_list[i]->sbtype==SHAPELET) {
			nmax = *(sb_list[i]->indxptr);
			source_npixels += nmax*nmax;
		}
	}
}

void QLens::initialize_pixel_matrices_shapelets(bool verbal)
{
	//if (source_pixel_vector != NULL) die("source surface brightness vector already initialized");
	vectorize_image_pixel_surface_brightness(true);
	count_shapelet_npixels();
	source_n_amps = source_npixels;
	if (include_imgfluxes_in_inversion) {
		for (int i=0; i < point_imgs.size(); i++) {
			source_n_amps += point_imgs[i].size(); // in this case, source amplitudes include point image amplitudes as well as pixel values
		}
	}

	point_image_surface_brightness = new double[image_npixels];

	if (source_n_amps <= 0) die("no shapelet amplitudes found");
	source_pixel_vector = new double[source_n_amps];
	if ((mpi_id==0) and (verbal)) cout << "Creating shapelet Lmatrix...\n";
	Lmatrix_dense.input(image_npixels,source_n_amps);
	Lmatrix_dense = 0;
	if (include_imgfluxes_in_inversion) {
		int nimgs = 0;
		for (int i=0; i < point_imgs.size(); i++) nimgs += point_imgs[i].size();
		Lmatrix_transpose_ptimg_amps.input(nimgs,image_npixels);
	}


	assign_Lmatrix_shapelets(verbal);
}

void QLens::clear_pixel_matrices()
{
	if (image_surface_brightness != NULL) delete[] image_surface_brightness;
	if (point_image_surface_brightness != NULL) delete[] point_image_surface_brightness;
	if (sbprofile_surface_brightness != NULL) delete[] sbprofile_surface_brightness;
	if (source_pixel_vector != NULL) delete[] source_pixel_vector;
	if (active_image_pixel_i != NULL) delete[] active_image_pixel_i;
	if (active_image_pixel_j != NULL) delete[] active_image_pixel_j;
	if (image_pixel_location_Lmatrix != NULL) delete[] image_pixel_location_Lmatrix;
	if (Lmatrix_index != NULL) delete[] Lmatrix_index;
	if (Lmatrix != NULL) delete[] Lmatrix;
	if (source_pixel_location_Lmatrix != NULL) delete[] source_pixel_location_Lmatrix;
	image_surface_brightness = NULL;
	point_image_surface_brightness = NULL;
	sbprofile_surface_brightness = NULL;
	source_pixel_vector = NULL;
	active_image_pixel_i = NULL;
	active_image_pixel_j = NULL;
	image_pixel_location_Lmatrix = NULL;
	source_pixel_location_Lmatrix = NULL;
	Lmatrix = NULL;
	Lmatrix_index = NULL;
	for (int i=0; i < image_pixel_grid->x_N; i++) {
		for (int j=0; j < image_pixel_grid->y_N; j++) {
			image_pixel_grid->mapped_cartesian_srcpixels[i][j].clear();
			image_pixel_grid->mapped_delaunay_srcpixels[i][j].clear();
		}
	}
	if ((n_image_prior) and (source_fit_mode==Cartesian_Source)) {
		if (source_pixel_n_images != NULL) delete[] source_pixel_n_images;
		source_pixel_n_images = NULL;
	}
}

/*
void QLens::clear_pixel_matrices_dense()
{
	if (image_surface_brightness != NULL) delete[] image_surface_brightness;
	if (sbprofile_surface_brightness != NULL) delete[] sbprofile_surface_brightness;
	if (source_pixel_vector != NULL) delete[] source_pixel_vector;
	if (active_image_pixel_i != NULL) delete[] active_image_pixel_i;
	if (active_image_pixel_j != NULL) delete[] active_image_pixel_j;
	image_surface_brightness = NULL;
	sbprofile_surface_brightness = NULL;
	source_pixel_vector = NULL;
	active_image_pixel_i = NULL;
	active_image_pixel_j = NULL;
}
*/

void QLens::assign_Lmatrix(const bool delaunay, const bool verbal)
{
	int img_index;
	int index;
	int i,j;
	Lmatrix_rows = new vector<double>[image_npixels];
	Lmatrix_index_rows = new vector<int>[image_npixels];
	int *Lmatrix_row_nn = new int[image_npixels];
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	if ((!delaunay) and (image_pixel_grid->ray_tracing_method == Area_Overlap))
	{
		lensvector *corners[4];
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			#pragma omp for private(img_index,i,j,index,corners) schedule(dynamic)
			for (img_index=0; img_index < image_npixels; img_index++) {
				index=0;
				i = active_image_pixel_i[img_index];
				j = active_image_pixel_j[img_index];
				corners[0] = &image_pixel_grid->corner_sourcepts[i][j];
				corners[1] = &image_pixel_grid->corner_sourcepts[i][j+1];
				corners[2] = &image_pixel_grid->corner_sourcepts[i+1][j];
				corners[3] = &image_pixel_grid->corner_sourcepts[i+1][j+1];
				source_pixel_grid->calculate_Lmatrix_overlap(img_index,i,j,index,corners,&image_pixel_grid->twist_pts[i][j],image_pixel_grid->twist_status[i][j],thread);
				Lmatrix_row_nn[img_index] = index;
			}
		}
	}
	else // interpolate
	{
		int max_nsplit = image_pixel_grid->max_nsplit;
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			int nsubpix,subcell_index;
			lensvector *center_srcpt, *center_pt;

			if (split_imgpixels) {
				#pragma omp for private(img_index,i,j,nsubpix,index,center_srcpt,center_pt) schedule(dynamic)
				for (img_index=0; img_index < image_npixels; img_index++) {
					index = 0;
					i = active_image_pixel_i[img_index];
					j = active_image_pixel_j[img_index];

					nsubpix = INTSQR(image_pixel_grid->nsplits[i][j]);
					center_srcpt = image_pixel_grid->subpixel_center_sourcepts[i][j];
					center_pt = image_pixel_grid->subpixel_center_pts[i][j];
					if (delaunay) {
						for (subcell_index=0; subcell_index < nsubpix; subcell_index++) {
							delaunay_srcgrid->calculate_Lmatrix(img_index,i,j,index,center_srcpt[subcell_index],subcell_index,1.0/nsubpix,thread);
						}
					} else {
						for (subcell_index=0; subcell_index < nsubpix; subcell_index++) {
							source_pixel_grid->calculate_Lmatrix_interpolate(img_index,i,j,index,center_srcpt[subcell_index],subcell_index,1.0/nsubpix,thread);
						}
					}
					Lmatrix_row_nn[img_index] = index;
				}
			} else {
				#pragma omp for private(img_index,i,j,index) schedule(dynamic)	
				for (img_index=0; img_index < image_npixels; img_index++) {
					index = 0;
					i = active_image_pixel_i[img_index];
					j = active_image_pixel_j[img_index];
					if (delaunay) {
						delaunay_srcgrid->calculate_Lmatrix(img_index,i,j,index,image_pixel_grid->center_sourcepts[i][j],0,1.0,thread);
					} else {
						source_pixel_grid->calculate_Lmatrix_interpolate(img_index,i,j,index,image_pixel_grid->center_sourcepts[i][j],0,1.0,thread);
					}
					Lmatrix_row_nn[img_index] = index;
				}
			}
		}
	}

	image_pixel_location_Lmatrix[0] = 0;
	for (img_index=0; img_index < image_npixels; img_index++) {
		image_pixel_location_Lmatrix[img_index+1] = image_pixel_location_Lmatrix[img_index] + Lmatrix_row_nn[img_index];
	}
	if (image_pixel_location_Lmatrix[img_index] != Lmatrix_n_elements) die("Number of Lmatrix elements don't match (%i vs %i)",image_pixel_location_Lmatrix[img_index],Lmatrix_n_elements);

	index=0;
	for (i=0; i < image_npixels; i++) {
		for (j=0; j < Lmatrix_row_nn[i]; j++) {
			Lmatrix[index] = Lmatrix_rows[i][j];
			Lmatrix_index[index] = Lmatrix_index_rows[i][j];
			index++;
		}
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for constructing Lmatrix: " << wtime << endl;
	}
#endif
	if ((mpi_id==0) and (verbal)) {
		int Lmatrix_ntot = source_n_amps*image_npixels;
		double sparseness = ((double) Lmatrix_n_elements)/Lmatrix_ntot;
		cout << "image has " << image_pixel_grid->n_active_pixels << " active pixels, Lmatrix has " << Lmatrix_n_elements << " nonzero elements (sparseness " << sparseness << ")\n";
	}

	delete[] Lmatrix_row_nn;
	delete[] Lmatrix_rows;
	delete[] Lmatrix_index_rows;
}

void QLens::assign_Lmatrix_shapelets(bool verbal)
{
	int img_index;
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	int i,j,k,n_shapelet_sets = 0;
	for (i=0; i < n_sb; i++) {
		if (sb_list[i]->sbtype==SHAPELET) n_shapelet_sets++;
	}
	if (n_shapelet_sets==0) return;

	SB_Profile** shapelet;
	shapelet = new SB_Profile*[n_shapelet_sets];
	bool at_least_one_lensed_shapelet = false;
	for (i=0,j=0; i < n_sb; i++) {
		if (sb_list[i]->sbtype==SHAPELET) {
			shapelet[j++] = sb_list[i];
			if (sb_list[i]->is_lensed) at_least_one_lensed_shapelet = true;
		}
	}

	#pragma omp parallel
	{
		int thread;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
		int nsubpix,subcell_index;
		lensvector *center_srcpt, *center_pt;

		if (split_imgpixels) {
			#pragma omp for private(img_index,i,j,k,nsubpix,center_srcpt,center_pt) schedule(dynamic)
			for (img_index=0; img_index < image_npixels; img_index++) {
				i = active_image_pixel_i[img_index];
				j = active_image_pixel_j[img_index];

				nsubpix = INTSQR(image_pixel_grid->nsplits[i][j]);
				center_srcpt = image_pixel_grid->subpixel_center_sourcepts[i][j];
				center_pt = image_pixel_grid->subpixel_center_pts[i][j];
				for (subcell_index=0; subcell_index < nsubpix; subcell_index++) {
					double *Lmatptr = Lmatrix_dense.subarray(img_index);
					for (k=0; k < n_shapelet_sets; k++) {
						if (shapelet[k]->is_lensed) {
							//Note that calculate_Lmatrix_elements(...) will increment Lmatptr as it goes
							shapelet[k]->calculate_Lmatrix_elements(center_srcpt[subcell_index][0],center_srcpt[subcell_index][1],Lmatptr,1.0/nsubpix);
							//shapelet[k]->calculate_Lmatrix_elements(image_pixel_grid->center_sourcepts[i][j][0],image_pixel_grid->center_sourcepts[i][j][1],Lmatptr,1.0/nsubpix);
							//cout << "cell " << i << "," << j << ": " << image_pixel_grid->center_sourcepts[i][j][0] << " " << image_pixel_grid->center_sourcepts[i][j][1] << " vs " << center_pt[subcell_index][0] << " " << center_pt[subcell_index][1] << endl;
						} else {
							shapelet[k]->calculate_Lmatrix_elements(center_pt[subcell_index][0],center_pt[subcell_index][1],Lmatptr,1.0/nsubpix);
						}
					}
				}
			}
		} else {
			lensvector center, center_srcpt;
			#pragma omp for private(img_index,i,j,center_srcpt) schedule(dynamic)
			for (img_index=0; img_index < image_npixels; img_index++) {
				i = active_image_pixel_i[img_index];
				j = active_image_pixel_j[img_index];

				double *Lmatptr = Lmatrix_dense.subarray(img_index);
				for (k=0; k < n_shapelet_sets; k++) {
					if (shapelet[k]->is_lensed) {
						center_srcpt = image_pixel_grid->center_sourcepts[i][j];
						shapelet[k]->calculate_Lmatrix_elements(center_srcpt[0],center_srcpt[1],Lmatptr,1.0);
					} else {
						center = image_pixel_grid->center_pts[i][j];
						shapelet[k]->calculate_Lmatrix_elements(center[0],center[1],Lmatptr,1.0);
					}
				}
			}
		}
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for constructing shapelet Lmatrix: " << wtime << endl;
	}
#endif
	delete[] shapelet;
}

void QLens::PSF_convolution_Lmatrix(bool verbal)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	if (psf_convolution_mpi) {
		MPI_Comm_create(*group_comm, *mpi_group, &sub_comm);
	}
#endif

	if (use_input_psf_matrix) {
		if (psf_matrix == NULL) return;
	}
	else if (generate_PSF_matrix(image_pixel_grid->pixel_xlength,image_pixel_grid->pixel_ylength)==false) return;
	if ((mpi_id==0) and (verbal)) cout << "Beginning PSF convolution...\n";
	double nx_half, ny_half;
	nx_half = psf_npixels_x/2;
	ny_half = psf_npixels_y/2;

	int *Lmatrix_psf_row_nn = new int[image_npixels];
	vector<double> *Lmatrix_psf_rows = new vector<double>[image_npixels];
	vector<int> *Lmatrix_psf_index_rows = new vector<int>[image_npixels];

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	// If the PSF is sufficiently wide, it may save time to MPI the PSF convolution by setting psf_convolution_mpi to 'true'. This option is off by default.
	int mpi_chunk, mpi_start, mpi_end;
	if (psf_convolution_mpi) {
		mpi_chunk = image_npixels / group_np;
		mpi_start = group_id*mpi_chunk;
		if (group_id == group_np-1) mpi_chunk += (image_npixels % group_np); // assign the remainder elements to the last mpi process
		mpi_end = mpi_start + mpi_chunk;
	} else {
		mpi_start = 0; mpi_end = image_npixels;
	}

	int i,j,k,l,m;
	int psf_k, psf_l;
	int img_index1, img_index2, src_index, col_index;
	int index;
	bool new_entry;
	int Lmatrix_psf_nn=0;
	int Lmatrix_psf_nn_part=0;
	#pragma omp parallel for private(m,k,l,i,j,img_index1,img_index2,src_index,col_index,psf_k,psf_l,index,new_entry) schedule(static) reduction(+:Lmatrix_psf_nn_part)
	for (img_index1=mpi_start; img_index1 < mpi_end; img_index1++)
	{ // this loops over columns of the PSF blurring matrix
		int col_i=0;
		Lmatrix_psf_row_nn[img_index1] = 0;
		k = active_image_pixel_i[img_index1];
		l = active_image_pixel_j[img_index1];
		for (psf_k=0; psf_k < psf_npixels_x; psf_k++) {
			i = k + nx_half - psf_k; // Note, 'k' is the index for the convolved image, so we have k = i - nx_half + psf_k
			if ((i >= 0) and (i < image_pixel_grid->x_N)) {
				for (psf_l=0; psf_l < psf_npixels_y; psf_l++) {
					j = l + ny_half - psf_l; // Note, 'l' is the index for the convolved image, so we have l = j - ny_half + psf_l
					if ((j >= 0) and (j < image_pixel_grid->y_N)) {
						if (image_pixel_grid->maps_to_source_pixel[i][j]) {
							img_index2 = image_pixel_grid->pixel_index[i][j];

							for (index=image_pixel_location_Lmatrix[img_index2]; index < image_pixel_location_Lmatrix[img_index2+1]; index++) {
								if (Lmatrix[index] != 0) {
									src_index = Lmatrix_index[index];
									new_entry = true;
									for (m=0; m < Lmatrix_psf_row_nn[img_index1]; m++) {
										if (Lmatrix_psf_index_rows[img_index1][m]==src_index) { col_index=m; new_entry=false; }
									}
									if (new_entry) {
										Lmatrix_psf_rows[img_index1].push_back(psf_matrix[psf_k][psf_l]*Lmatrix[index]);
										Lmatrix_psf_index_rows[img_index1].push_back(src_index);
										Lmatrix_psf_row_nn[img_index1]++;
										col_i++;
									} else {
										Lmatrix_psf_rows[img_index1][col_index] += psf_matrix[psf_k][psf_l]*Lmatrix[index];
									}
								}
							}
						}
					}
				}
			}
		}
		Lmatrix_psf_nn_part += col_i;
	}

#ifdef USE_MPI
	if (psf_convolution_mpi)
		MPI_Allreduce(&Lmatrix_psf_nn_part, &Lmatrix_psf_nn, 1, MPI_INT, MPI_SUM, sub_comm);
	else
		Lmatrix_psf_nn = Lmatrix_psf_nn_part;
#else
	Lmatrix_psf_nn = Lmatrix_psf_nn_part;
#endif

	int *image_pixel_location_Lmatrix_psf = new int[image_npixels+1];

#ifdef USE_MPI
	if (psf_convolution_mpi) {
		int id, chunk, start, end, length;
		for (id=0; id < group_np; id++) {
			chunk = image_npixels / group_np;
			start = id*chunk;
			if (id == group_np-1) chunk += (image_npixels % group_np); // assign the remainder elements to the last mpi process
			MPI_Bcast(Lmatrix_psf_row_nn + start,chunk,MPI_INT,id,sub_comm);
		}
	}
#endif

	if (include_imgfluxes_in_inversion) {
		double *Lmatptr;
		i=0;
		for (j=0; j < point_imgs.size(); j++) {
			for (k=0; k < point_imgs[j].size(); k++) {
				Lmatptr = Lmatrix_transpose_ptimg_amps.subarray(i);
				image_pixel_grid->generate_point_images(point_imgs[j], Lmatptr, false, -1, k);
				i++;
			}
		}
		int src_amp_i;
		double *Lmatrix_transpose_line;
		i=0;
		for (j=0; j < point_imgs.size(); j++) {
			for (k=0; k < point_imgs[j].size(); k++) {
				src_amp_i = source_npixels + i;
				Lmatrix_transpose_line = Lmatrix_transpose_ptimg_amps[i];
				for (int img_index=0; img_index < image_npixels; img_index++) {
					if (Lmatrix_transpose_line[img_index] != 0) {
						Lmatrix_psf_rows[img_index].push_back(Lmatrix_transpose_line[img_index]);
						Lmatrix_psf_index_rows[img_index].push_back(src_amp_i);
						Lmatrix_psf_row_nn[img_index]++;
						Lmatrix_psf_nn++;
					}
				}
				i++;
			}
		}
	}

	image_pixel_location_Lmatrix_psf[0] = 0;
	for (m=0; m < image_npixels; m++) {
		image_pixel_location_Lmatrix_psf[m+1] = image_pixel_location_Lmatrix_psf[m] + Lmatrix_psf_row_nn[m];
	}


	double *Lmatrix_psf = new double[Lmatrix_psf_nn];
	int *Lmatrix_index_psf = new int[Lmatrix_psf_nn];

	int indx;
	for (m=mpi_start; m < mpi_end; m++) {
		indx = image_pixel_location_Lmatrix_psf[m];
		for (j=0; j < Lmatrix_psf_row_nn[m]; j++) {
			Lmatrix_psf[indx+j] = Lmatrix_psf_rows[m][j];
			Lmatrix_index_psf[indx+j] = Lmatrix_psf_index_rows[m][j];
		}
	}

#ifdef USE_MPI
	if (psf_convolution_mpi) {
		int id, chunk, start, end, length;
		for (id=0; id < group_np; id++) {
			chunk = image_npixels / group_np;
			start = id*chunk;
			if (id == group_np-1) chunk += (image_npixels % group_np); // assign the remainder elements to the last mpi process
			end = start + chunk;
			length = image_pixel_location_Lmatrix_psf[end] - image_pixel_location_Lmatrix_psf[start];
			MPI_Bcast(Lmatrix_psf + image_pixel_location_Lmatrix_psf[start],length,MPI_DOUBLE,id,sub_comm);
			MPI_Bcast(Lmatrix_index_psf + image_pixel_location_Lmatrix_psf[start],length,MPI_INT,id,sub_comm);
		}
		MPI_Comm_free(&sub_comm);
	}
#endif

	if ((mpi_id==0) and (verbal)) cout << "Lmatrix after PSF convolution: Lmatrix now has " << indx << " nonzero elements\n";

	delete[] Lmatrix;
	delete[] Lmatrix_index;
	delete[] image_pixel_location_Lmatrix;
	Lmatrix = Lmatrix_psf;
	Lmatrix_index = Lmatrix_index_psf;
	image_pixel_location_Lmatrix = image_pixel_location_Lmatrix_psf;
	Lmatrix_n_elements = Lmatrix_psf_nn;

	delete[] Lmatrix_psf_row_nn;
	delete[] Lmatrix_psf_rows;
	delete[] Lmatrix_psf_index_rows;

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating PSF convolution of Lmatrix: " << wtime << endl;
	}
#endif
}

void QLens::convert_Lmatrix_to_dense()
{
	int i,j;
	Lmatrix_dense.input(image_npixels,source_n_amps);
	Lmatrix_dense = 0;
	for (i=0; i < image_npixels; i++) {
		for (j=image_pixel_location_Lmatrix[i]; j < image_pixel_location_Lmatrix[i+1]; j++) {
			Lmatrix_dense[i][Lmatrix_index[j]] += Lmatrix[j];
		}
	}
}

bool QLens::setup_convolution_FFT(const bool verbal)
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int npix;
	int *pixel_map_i, *pixel_map_j;
	npix = image_npixels;
	pixel_map_i = active_image_pixel_i;
	pixel_map_j = active_image_pixel_j;
	if (use_input_psf_matrix) {
		if (psf_matrix == NULL) return false;
	}
	else {
		if ((psf_width_x==0) and (psf_width_y==0)) return false;
		else if (generate_PSF_matrix(image_pixel_grid->pixel_xlength,image_pixel_grid->pixel_ylength)==false) {
			if (verbal) warn("could not generate_PSF matrix");
			return false;
		}
	}
	int nx_half, ny_half;
	nx_half = psf_npixels_x/2;
	ny_half = psf_npixels_y/2;

	int i,j,k,img_index;
	fft_ni = 1;
	fft_nj = 1;
	fft_imin = 50000;
	fft_jmin = 50000;
	int imax=-1,jmax=-1;
	int il0, jl0;

	for (img_index=0; img_index < npix; img_index++)
	{
		i = pixel_map_i[img_index];
		j = pixel_map_j[img_index];
		if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j]))) {
			if (i > imax) imax = i;
			if (j > jmax) jmax = j;
			if (i < fft_imin) fft_imin = i;
			if (j < fft_jmin) fft_jmin = j;
		}
	}
	il0 = 1+imax-fft_imin + psf_npixels_x; // will pad with extra zeros to avoid edge effects (wraparound of PSF blurring)
	jl0 = 1+jmax-fft_jmin + psf_npixels_y;

#ifdef USE_FFTW
	fft_ni = il0;
	fft_nj = jl0;
	if (fft_ni % 2 != 0) fft_ni++;
	if (fft_nj % 2 != 0) fft_nj++;
	int ncomplex = fft_nj*(fft_ni/2+1);
	int npix_conv = fft_ni*fft_nj;
	double *psf_rvec = new double[npix_conv];
	psf_transform = new complex<double>[ncomplex];
	fftw_plan fftplan_psf = fftw_plan_dft_r2c_2d(fft_nj,fft_ni,psf_rvec,reinterpret_cast<fftw_complex*>(psf_transform),FFTW_MEASURE);
	for (i=0; i < npix_conv; i++) psf_rvec[i] = 0;
	img_rvec = new double[npix_conv];
	img_transform = new complex<double>[ncomplex];
	fftplan = fftw_plan_dft_r2c_2d(fft_nj,fft_ni,img_rvec,reinterpret_cast<fftw_complex*>(img_transform),FFTW_MEASURE);
	fftplan_inverse = fftw_plan_dft_c2r_2d(fft_nj,fft_ni,reinterpret_cast<fftw_complex*>(img_transform),img_rvec,FFTW_MEASURE);
	for (i=0; i < npix_conv; i++) img_rvec[i] = 0;

	Lmatrix_imgs_rvec = new double*[source_npixels];
	Lmatrix_transform = new complex<double>*[source_npixels];
	fftplans_Lmatrix = new fftw_plan[source_npixels];
	fftplans_Lmatrix_inverse = new fftw_plan[source_npixels];
	for (i=0; i < source_npixels; i++) {
		Lmatrix_imgs_rvec[i] = new double[npix_conv];
		Lmatrix_transform[i] = new complex<double>[ncomplex];
		fftplans_Lmatrix[i] = fftw_plan_dft_r2c_2d(fft_nj,fft_ni,Lmatrix_imgs_rvec[i],reinterpret_cast<fftw_complex*>(Lmatrix_transform[i]),FFTW_MEASURE);
		fftplans_Lmatrix_inverse[i] = fftw_plan_dft_c2r_2d(fft_nj,fft_ni,reinterpret_cast<fftw_complex*>(Lmatrix_transform[i]),Lmatrix_imgs_rvec[i],FFTW_MEASURE);
		for (j=0; j < npix_conv; j++) Lmatrix_imgs_rvec[i][j] = 0;
	}
#else
	while (fft_ni < il0) fft_ni *= 2; // need multiple of 2 to do FFT (not necessary with FFTW, but still seems faster that way)
	while (fft_nj < jl0) fft_nj *= 2; // need multiple of 2 to do FFT (not necessary with FFTW, but still seems faster that way)
	psf_zvec = new double[2*fft_ni*fft_nj];
	for (i=0; i < 2*fft_ni*fft_nj; i++) psf_zvec[i] = 0;
#endif
	int zpsf_i, zpsf_j;
	int l;
	for (i=-nx_half; i < psf_npixels_x - nx_half; i++) {
		for (j=-ny_half; j < psf_npixels_y - ny_half; j++) {
			zpsf_i=i;
			zpsf_j=j;
			if (zpsf_i < 0) zpsf_i += fft_ni;
			if (zpsf_j < 0) zpsf_j += fft_nj;
#ifdef USE_FFTW
			l = zpsf_j*fft_ni + zpsf_i;
			psf_rvec[l] = psf_matrix[nx_half+i][ny_half+j];
#else
			k = 2*(zpsf_j*fft_ni + zpsf_i);
			psf_zvec[k] = psf_matrix[nx_half+i][ny_half+j];
#endif
		}
	}

#ifdef USE_FFTW
	fftw_execute(fftplan_psf);
	fftw_destroy_plan(fftplan_psf);
	delete[] psf_rvec;
#else
	int nnvec[2];
	nnvec[0] = fft_ni;
	nnvec[1] = fft_nj;
	fourier_transform(psf_zvec,2,nnvec,1);
#endif
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) {
			cout << "Wall time for setting up FFT for convolutions: " << wtime << endl;
		}
	}
#endif

	setup_fft_convolution = true;
	return true;
}

bool QLens::setup_convolution_FFT_emask(const bool verbal)
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int npix;
	int *pixel_map_i, *pixel_map_j;
	npix = image_npixels;
	pixel_map_i = active_image_pixel_i;
	pixel_map_j = active_image_pixel_j;
	if (use_input_psf_matrix) {
		if (psf_matrix == NULL) return false;
	}
	else {
		if ((psf_width_x==0) and (psf_width_y==0)) return false;
		else if (generate_PSF_matrix(image_pixel_grid->pixel_xlength,image_pixel_grid->pixel_ylength)==false) {
			if (verbal) warn("could not generate_PSF matrix");
			return false;
		}
	}
	int nx_half, ny_half;
	nx_half = psf_npixels_x/2;
	ny_half = psf_npixels_y/2;

	int i,j,k,img_index;
	fft_ni_emask = 1;
	fft_nj_emask = 1;
	fft_imin_emask = 50000;
	fft_jmin_emask = 50000;
	int imax=-1,jmax=-1;
	int il0, jl0;

	for (img_index=0; img_index < npix; img_index++)
	{
		i = pixel_map_i[img_index];
		j = pixel_map_j[img_index];
		if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j]))) {
			if (i > imax) imax = i;
			if (j > jmax) jmax = j;
			if (i < fft_imin_emask) fft_imin_emask = i;
			if (j < fft_jmin_emask) fft_jmin_emask = j;
		}
	}
	il0 = 1+imax-fft_imin_emask + psf_npixels_x; // will pad with extra zeros to avoid edge effects (wraparound of PSF blurring)
	jl0 = 1+jmax-fft_jmin_emask + psf_npixels_y;

#ifdef USE_FFTW
	fft_ni_emask = il0;
	fft_nj_emask = jl0;
	if (fft_ni_emask % 2 != 0) fft_ni_emask++;
	if (fft_nj_emask % 2 != 0) fft_nj_emask++;
	int ncomplex = fft_nj_emask*(fft_ni_emask/2+1);
	int npix_conv = fft_ni_emask*fft_nj_emask;
	double *psf_rvec = new double[npix_conv];
	//psf_transform_emask = (fftw_complex*) fftw_malloc(sizeof(fftw_complex)*ncomplex);
	psf_transform_emask = new complex<double>[ncomplex];
	fftw_plan fftplan_psf_emask = fftw_plan_dft_r2c_2d(fft_nj_emask,fft_ni_emask,psf_rvec,reinterpret_cast<fftw_complex*>(psf_transform_emask),FFTW_MEASURE);
	for (i=0; i < npix_conv; i++) psf_rvec[i] = 0;
	//img_rvec = new double[npix_conv];
	//img_transform = new complex<double>[ncomplex];
	//fftplan = fftw_plan_dft_r2c_2d(fft_nj_emask,fft_ni_emask,img_rvec,reinterpret_cast<fftw_complex*>(img_transform),FFTW_MEASURE);
	//fftplan_inverse = fftw_plan_dft_c2r_2d(fft_nj_emask,fft_ni_emask,reinterpret_cast<fftw_complex*>(img_transform),img_rvec,FFTW_MEASURE);
	//for (i=0; i < npix_conv; i++) img_rvec[i] = 0;

	Lmatrix_imgs_rvec_emask = new double*[source_npixels];
	Lmatrix_transform_emask = new complex<double>*[source_npixels];
	fftplans_Lmatrix_emask = new fftw_plan[source_npixels];
	fftplans_Lmatrix_inverse_emask = new fftw_plan[source_npixels];
	for (i=0; i < source_npixels; i++) {
		Lmatrix_imgs_rvec_emask[i] = new double[npix_conv];
		Lmatrix_transform_emask[i] = new complex<double>[ncomplex];
		fftplans_Lmatrix_emask[i] = fftw_plan_dft_r2c_2d(fft_nj_emask,fft_ni_emask,Lmatrix_imgs_rvec_emask[i],reinterpret_cast<fftw_complex*>(Lmatrix_transform_emask[i]),FFTW_MEASURE);
		fftplans_Lmatrix_inverse_emask[i] = fftw_plan_dft_c2r_2d(fft_nj_emask,fft_ni_emask,reinterpret_cast<fftw_complex*>(Lmatrix_transform_emask[i]),Lmatrix_imgs_rvec_emask[i],FFTW_MEASURE);
		for (j=0; j < npix_conv; j++) Lmatrix_imgs_rvec_emask[i][j] = 0;
	}
#else
	while (fft_ni_emask < il0) fft_ni_emask *= 2; // need multiple of 2 to do FFT (not necessary with FFTW, but still seems faster that way)
	while (fft_nj_emask < jl0) fft_nj_emask *= 2; // need multiple of 2 to do FFT (not necessary with FFTW, but still seems faster that way)
	psf_zvec_emask = new double[2*fft_ni_emask*fft_nj_emask];
	for (i=0; i < 2*fft_ni_emask*fft_nj_emask; i++) psf_zvec_emask[i] = 0;
#endif
	int zpsf_i, zpsf_j;
	int l;
	for (i=-nx_half; i < psf_npixels_x - nx_half; i++) {
		for (j=-ny_half; j < psf_npixels_y - ny_half; j++) {
			zpsf_i=i;
			zpsf_j=j;
			if (zpsf_i < 0) zpsf_i += fft_ni_emask;
			if (zpsf_j < 0) zpsf_j += fft_nj_emask;
#ifdef USE_FFTW
			l = zpsf_j*fft_ni_emask + zpsf_i;
			psf_rvec[l] = psf_matrix[nx_half+i][ny_half+j];
#else
			k = 2*(zpsf_j*fft_ni_emask + zpsf_i);
			psf_zvec_emask[k] = psf_matrix[nx_half+i][ny_half+j];
#endif
		}
	}

#ifdef USE_FFTW
	fftw_execute(fftplan_psf_emask);
	fftw_destroy_plan(fftplan_psf_emask);
	delete[] psf_rvec;
#else
	int nnvec[2];
	nnvec[0] = fft_ni_emask;
	nnvec[1] = fft_nj_emask;
	fourier_transform(psf_zvec_emask,2,nnvec,1);
#endif
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) {
			cout << "Wall time for setting up FFT for convolutions: " << wtime << endl;
		}
	}
#endif

	setup_fft_convolution_emask = true;
	return true;
}

void QLens::cleanup_FFT_convolution_arrays()
{
	if (setup_fft_convolution) {
#ifdef USE_FFTW
		delete[] psf_transform;
		psf_transform = NULL;
		for (int i=0; i < source_npixels; i++) {
			delete[] Lmatrix_imgs_rvec[i];
			delete[] Lmatrix_transform[i];
			fftw_destroy_plan(fftplans_Lmatrix[i]);
			fftw_destroy_plan(fftplans_Lmatrix_inverse[i]);
		}
		delete[] Lmatrix_imgs_rvec;
		delete[] Lmatrix_transform;
		delete[] fftplans_Lmatrix;
		delete[] fftplans_Lmatrix_inverse;
		fftw_destroy_plan(fftplan);
		fftw_destroy_plan(fftplan_inverse);
#else
		delete[] psf_zvec;
#endif
		fft_imin=fft_jmin=fft_ni=fft_nj=0;
		setup_fft_convolution = false;
	}
	if (setup_fft_convolution_emask) {
#ifdef USE_FFTW
		delete[] psf_transform_emask;
		psf_transform = NULL;
		for (int i=0; i < source_npixels; i++) {
			delete[] Lmatrix_imgs_rvec_emask[i];
			delete[] Lmatrix_transform_emask[i];
			fftw_destroy_plan(fftplans_Lmatrix_emask[i]);
			fftw_destroy_plan(fftplans_Lmatrix_inverse_emask[i]);
		}
		delete[] Lmatrix_imgs_rvec_emask;
		delete[] Lmatrix_transform_emask;
		delete[] fftplans_Lmatrix_emask;
		delete[] fftplans_Lmatrix_inverse_emask;
#else
		delete[] psf_zvec_emask;
#endif
		fft_imin_emask=fft_jmin_emask=fft_ni_emask=fft_nj_emask=0;
		setup_fft_convolution_emask = false;
	}
}

void QLens::PSF_convolution_Lmatrix_dense(const bool verbal)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	if (psf_convolution_mpi) {
		MPI_Comm_create(*group_comm, *mpi_group, &sub_comm);
	}
#endif

	if (source_npixels > 0) {
		if ((mpi_id==0) and (verbal)) cout << "Beginning PSF convolution...\n";

		double nx_half, ny_half;
		nx_half = psf_npixels_x/2;
		ny_half = psf_npixels_y/2;

		if (fft_convolution) {
			if (!setup_fft_convolution) {
				if (!setup_convolution_FFT(verbal)) {
					warn("PSF convolution FFT failed");
					return;	
				}
			}
			int *pixel_map_i, *pixel_map_j;
			pixel_map_i = active_image_pixel_i;
			pixel_map_j = active_image_pixel_j;

			int i,j,k,l,img_index;

#ifdef USE_FFTW
			int ncomplex = fft_nj*(fft_ni/2+1);
#else
			int nnvec[2];
			nnvec[0] = fft_ni;
			nnvec[1] = fft_nj;
			int nzvec = 2*fft_ni*fft_nj;
			double **Lmatrix_imgs_zvec = new double*[source_npixels];
			for (i=0; i < source_npixels; i++) {
				Lmatrix_imgs_zvec[i] = new double[nzvec];
			}
#endif


#ifdef USE_OPENMP
			if (show_wtime) {
				wtime0 = omp_get_wtime();
			}
#endif
			double fwtime0, fwtime;
			double rtemp, itemp;
			int npix_conv = fft_ni*fft_nj;
			double *img_zvec, *img_rvec;
			complex<double> *img_cvec;
			int src_index;
			#pragma omp parallel for private(k,i,j,l,img_index,src_index,img_zvec,img_rvec,img_cvec,rtemp,itemp) schedule(static)
			for (src_index=0; src_index < source_npixels; src_index++) {
#ifdef USE_FFTW
				img_rvec = Lmatrix_imgs_rvec[src_index];
				img_cvec = Lmatrix_transform[src_index];
				for (i=0; i < npix_conv; i++) img_rvec[i] = 0;
#else
				img_zvec = Lmatrix_imgs_zvec[src_index];
				for (j=0; j < nzvec; j++) img_zvec[j] = 0;
#endif
				for (img_index=0; img_index < image_npixels; img_index++)
				{
					i = pixel_map_i[img_index];
					j = pixel_map_j[img_index];
					if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j]))) {
						i -= fft_imin;
						j -= fft_jmin;
#ifdef USE_FFTW
						l = j*fft_ni + i;
						img_rvec[l] = Lmatrix_dense[img_index][src_index];
#else
						k = 2*(j*fft_ni + i);
						img_zvec[k] = Lmatrix_dense[img_index][src_index];
#endif

					}
				}

#ifdef USE_FFTW
				fftw_execute(fftplans_Lmatrix[src_index]);
				for (i=0; i < ncomplex; i++) {
					img_cvec[i] = img_cvec[i]*psf_transform[i];
					img_cvec[i] /= npix_conv;
				}
				fftw_execute(fftplans_Lmatrix_inverse[src_index]);

#else
				fourier_transform(img_zvec,2,nnvec,1);
				for (i=0,j=0; i < npix_conv; i++, j += 2) {
					rtemp = (img_zvec[j]*psf_zvec[j] - img_zvec[j+1]*psf_zvec[j+1]) / npix_conv;
					itemp = (img_zvec[j]*psf_zvec[j+1] + img_zvec[j+1]*psf_zvec[j]) / npix_conv;
					img_zvec[j] = rtemp;
					img_zvec[j+1] = itemp;
				}
				fourier_transform(img_zvec,2,nnvec,-1);
#endif

				for (img_index=0; img_index < image_npixels; img_index++)
				{
					i = pixel_map_i[img_index];
					j = pixel_map_j[img_index];
					if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j]))) {
						i -= fft_imin;
						j -= fft_jmin;
#ifdef USE_FFTW
						l = j*fft_ni + i;
						Lmatrix_dense[img_index][src_index] = img_rvec[l];
#else
						k = 2*(j*fft_ni + i);
						Lmatrix_dense[img_index][src_index] = img_zvec[k];
#endif
					}
				}
			}
#ifdef USE_OPENMP
			if (show_wtime) {
				wtime = omp_get_wtime() - wtime0;
				if (mpi_id==0) {
					cout << "Wall time for calculating PSF convolution of Lmatrix via FFT: " << wtime << endl;
				}
			}
#endif
#ifndef USE_FFTW
			for (i=0; i < source_npixels; i++) delete[] Lmatrix_imgs_zvec[i];
			delete[] Lmatrix_imgs_zvec;
#endif
		} else {
			if (use_input_psf_matrix) {
				if (psf_matrix == NULL) return;
			}
			else if (generate_PSF_matrix(image_pixel_grid->pixel_xlength,image_pixel_grid->pixel_ylength)==false) return;

			double **Lmatrix_psf = new double*[image_npixels];
			int i,j;
			for (i=0; i < image_npixels; i++) {
				Lmatrix_psf[i] = new double[source_npixels];
				for (j=0; j < source_npixels; j++) Lmatrix_psf[i][j] = 0;
			}

#ifdef USE_OPENMP
			if (show_wtime) {
				wtime0 = omp_get_wtime();
			}
#endif

			int k,l;
			int psf_k, psf_l;
			int img_index1, img_index2, src_index, col_index;
			int index;
			bool new_entry;
			int Lmatrix_psf_nn=0;
			int Lmatrix_psf_nn_part=0;
			double *lmatptr, *lmatpsfptr, psfval;
			#pragma omp parallel for private(k,l,i,j,img_index1,img_index2,src_index,psf_k,psf_l,lmatptr,lmatpsfptr,psfval) schedule(static) reduction(+:Lmatrix_psf_nn_part)
			for (img_index1=0; img_index1 < image_npixels; img_index1++)
			{ // this loops over columns of the PSF blurring matrix
				int col_i=0;
				k = active_image_pixel_i[img_index1];
				l = active_image_pixel_j[img_index1];
				for (psf_k=0; psf_k < psf_npixels_x; psf_k++) {
					i = k + nx_half - psf_k; // Note, 'k' is the index for the convolved image, so we have k = i - nx_half + psf_k
					if ((i >= 0) and (i < image_pixel_grid->x_N)) {
						for (psf_l=0; psf_l < psf_npixels_y; psf_l++) {
							j = l + ny_half - psf_l; // Note, 'l' is the index for the convolved image, so we have l = j - ny_half + psf_l
							psfval = psf_matrix[psf_k][psf_l];
							if ((j >= 0) and (j < image_pixel_grid->y_N)) {
								if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j]))) {
									img_index2 = image_pixel_grid->pixel_index[i][j];
									lmatptr = Lmatrix_dense.subarray(img_index2);
									lmatpsfptr = Lmatrix_psf[img_index1];
									for (src_index=0; src_index < source_npixels; src_index++) {
										(*(lmatpsfptr++)) += psfval*(*(lmatptr++));
									}
								}
							}
						}
					}
				}
			}

			// note, the following function sets the pointer in Lmatrix dense to Lmatrix_psf (and deletes the old pointer), so no garbage collection necessary afterwards
			Lmatrix_dense.input(Lmatrix_psf);


#ifdef USE_OPENMP
			if (show_wtime) {
				wtime = omp_get_wtime() - wtime0;
				if (mpi_id==0) cout << "Wall time for calculating dense PSF convolution of Lmatrix: " << wtime << endl;
			}
#endif
		}
	}
	if (include_imgfluxes_in_inversion) {
		int i,j,k;
		double *Lmatptr;
		i=0;
		for (j=0; j < point_imgs.size(); j++) {
			for (k=0; k < point_imgs[j].size(); k++) {
				Lmatptr = Lmatrix_transpose_ptimg_amps.subarray(i);
				image_pixel_grid->generate_point_images(point_imgs[j], Lmatptr, false, -1, k);
				i++;
			}
		}
		int src_amp_i;
		double *Lmatrix_transpose_line;
		i=0;
		for (j=0; j < point_imgs.size(); j++) {
			for (k=0; k < point_imgs[j].size(); k++) {
				src_amp_i = source_npixels + i;
				Lmatrix_transpose_line = Lmatrix_transpose_ptimg_amps[i];
				for (int img_index=0; img_index < image_npixels; img_index++) {
					Lmatrix_dense[img_index][src_amp_i] = Lmatrix_transpose_line[img_index];
				}
				i++;
			}
		}
	}
}

void QLens::PSF_convolution_Lmatrix_dense_emask(const bool verbal)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	if (psf_convolution_mpi) {
		MPI_Comm_create(*group_comm, *mpi_group, &sub_comm);
	}
#endif

	if ((mpi_id==0) and (verbal)) cout << "Beginning PSF convolution...\n";

	double nx_half, ny_half;
	nx_half = psf_npixels_x/2;
	ny_half = psf_npixels_y/2;

	if (fft_convolution) {
		if (!setup_fft_convolution_emask) {
			if (!setup_convolution_FFT_emask(verbal)) {
				warn("PSF convolution FFT failed for emask");
				return;	
			}
		}
		int *pixel_map_i, *pixel_map_j;
		pixel_map_i = active_image_pixel_i;
		pixel_map_j = active_image_pixel_j;

		int i,j,k,l,img_index;

#ifdef USE_FFTW
		int ncomplex = fft_nj_emask*(fft_ni_emask/2+1);
#else
		int nnvec[2];
		nnvec[0] = fft_ni_emask;
		nnvec[1] = fft_nj_emask;
		int nzvec = 2*fft_ni_emask*fft_nj_emask;
		double **Lmatrix_imgs_zvec_emask = new double*[source_npixels];
		for (i=0; i < source_npixels; i++) {
			Lmatrix_imgs_zvec_emask[i] = new double[nzvec];
		}
#endif


#ifdef USE_OPENMP
		if (show_wtime) {
			wtime0 = omp_get_wtime();
		}
#endif
		double fwtime0, fwtime;
		double rtemp, itemp;
		int npix_conv = fft_ni_emask*fft_nj_emask;
		double *img_zvec, *img_rvec_emask;
		complex<double> *img_cvec;
		int src_index;
		#pragma omp parallel for private(k,i,j,l,img_index,src_index,img_zvec,img_rvec_emask,img_cvec,rtemp,itemp) schedule(static)
		for (src_index=0; src_index < source_npixels; src_index++) {
#ifdef USE_FFTW
			img_rvec_emask = Lmatrix_imgs_rvec_emask[src_index];
			img_cvec = Lmatrix_transform_emask[src_index];
			for (i=0; i < npix_conv; i++) img_rvec_emask[i] = 0;
#else
			img_zvec = Lmatrix_imgs_zvec_emask[src_index];
			for (j=0; j < nzvec; j++) img_zvec[j] = 0;
#endif
			for (img_index=0; img_index < image_npixels; img_index++)
			{
				i = pixel_map_i[img_index];
				j = pixel_map_j[img_index];
				if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j]))) {
					i -= fft_imin_emask;
					j -= fft_jmin_emask;
#ifdef USE_FFTW
					l = j*fft_ni_emask + i;
					img_rvec_emask[l] = Lmatrix_dense[img_index][src_index];
#else
					k = 2*(j*fft_ni_emask + i);
					img_zvec[k] = Lmatrix_dense[img_index][src_index];
#endif
				}
			}

#ifdef USE_FFTW
			fftw_execute(fftplans_Lmatrix_emask[src_index]);
			for (i=0; i < ncomplex; i++) {
				img_cvec[i] = img_cvec[i]*psf_transform_emask[i];
				img_cvec[i] /= npix_conv;
			}
			fftw_execute(fftplans_Lmatrix_inverse_emask[src_index]);

#else
			fourier_transform(img_zvec,2,nnvec,1);
			for (i=0,j=0; i < npix_conv; i++, j += 2) {
				rtemp = (img_zvec[j]*psf_zvec_emask[j] - img_zvec[j+1]*psf_zvec_emask[j+1]) / npix_conv;
				itemp = (img_zvec[j]*psf_zvec_emask[j+1] + img_zvec[j+1]*psf_zvec_emask[j]) / npix_conv;
				img_zvec[j] = rtemp;
				img_zvec[j+1] = itemp;
			}
			fourier_transform(img_zvec,2,nnvec,-1);
#endif

			for (img_index=0; img_index < image_npixels; img_index++)
			{
				i = pixel_map_i[img_index];
				j = pixel_map_j[img_index];
				if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j]))) {
					i -= fft_imin_emask;
					j -= fft_jmin_emask;
#ifdef USE_FFTW
					l = j*fft_ni_emask + i;
					Lmatrix_dense[img_index][src_index] = img_rvec_emask[l];
#else
					k = 2*(j*fft_ni_emask + i);
					Lmatrix_dense[img_index][src_index] = img_zvec[k];
#endif
				}
			}
		}
#ifdef USE_OPENMP
		if (show_wtime) {
			wtime = omp_get_wtime() - wtime0;
			if (mpi_id==0) {
				cout << "Wall time for calculating PSF convolution of Lmatrix via FFT: " << wtime << endl;
			}
		}
#endif
#ifndef USE_FFTW
		for (i=0; i < source_npixels; i++) delete[] Lmatrix_imgs_zvec_emask[i];
		delete[] Lmatrix_imgs_zvec_emask;
#endif
	} else {
		if (use_input_psf_matrix) {
			if (psf_matrix == NULL) return;
		}
		else if (generate_PSF_matrix(image_pixel_grid->pixel_xlength,image_pixel_grid->pixel_ylength)==false) return;

		double **Lmatrix_psf = new double*[image_npixels];
		int i,j;
		for (i=0; i < image_npixels; i++) {
			Lmatrix_psf[i] = new double[source_npixels];
			for (j=0; j < source_npixels; j++) Lmatrix_psf[i][j] = 0;
		}

#ifdef USE_OPENMP
		if (show_wtime) {
			wtime0 = omp_get_wtime();
		}
#endif

		int k,l;
		int psf_k, psf_l;
		int img_index1, img_index2, src_index, col_index;
		int index;
		bool new_entry;
		int Lmatrix_psf_nn=0;
		int Lmatrix_psf_nn_part=0;
		double *lmatptr, *lmatpsfptr, psfval;
		#pragma omp parallel for private(k,l,i,j,img_index1,img_index2,src_index,psf_k,psf_l,lmatptr,lmatpsfptr,psfval) schedule(static) reduction(+:Lmatrix_psf_nn_part)
		for (img_index1=0; img_index1 < image_npixels; img_index1++)
		{ // this loops over columns of the PSF blurring matrix
			int col_i=0;
			k = active_image_pixel_i[img_index1];
			l = active_image_pixel_j[img_index1];
			for (psf_k=0; psf_k < psf_npixels_x; psf_k++) {
				i = k + nx_half - psf_k; // Note, 'k' is the index for the convolved image, so we have k = i - nx_half + psf_k
				if ((i >= 0) and (i < image_pixel_grid->x_N)) {
					for (psf_l=0; psf_l < psf_npixels_y; psf_l++) {
						j = l + ny_half - psf_l; // Note, 'l' is the index for the convolved image, so we have l = j - ny_half + psf_l
						psfval = psf_matrix[psf_k][psf_l];
						if ((j >= 0) and (j < image_pixel_grid->y_N)) {
							if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j]))) {
								img_index2 = image_pixel_grid->pixel_index[i][j];
								lmatptr = Lmatrix_dense.subarray(img_index2);
								lmatpsfptr = Lmatrix_psf[img_index1];
								for (src_index=0; src_index < source_npixels; src_index++) {
									(*(lmatpsfptr++)) += psfval*(*(lmatptr++));
								}
							}
						}
					}
				}
			}
		}

		// note, the following function sets the pointer in Lmatrix dense to Lmatrix_psf (and deletes the old pointer), so no garbage collection necessary afterwards
		Lmatrix_dense.input(Lmatrix_psf);

#ifdef USE_OPENMP
		if (show_wtime) {
			wtime = omp_get_wtime() - wtime0;
			if (mpi_id==0) cout << "Wall time for calculating dense PSF convolution of Lmatrix: " << wtime << endl;
		}
#endif
	}
}

void QLens::PSF_convolution_pixel_vector(double *surface_brightness_vector, const bool foreground, const bool verbal)
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	if ((mpi_id==0) and (verbal)) cout << "Beginning PSF convolution...\n";

	int nx_half, ny_half;

	if ((!foreground) and (fft_convolution)) {
		nx_half = psf_npixels_x/2;
		ny_half = psf_npixels_y/2;
		if (!setup_fft_convolution) {
			if (!setup_convolution_FFT(verbal)) {
				warn("PSF convolution FFT failed");
				return;	
			}
		}
		int *pixel_map_i, *pixel_map_j;
		pixel_map_i = active_image_pixel_i;
		pixel_map_j = active_image_pixel_j;

		int i,j,k,img_index;

#ifdef USE_FFTW
		int ncomplex = fft_nj*(fft_ni/2+1);
#else
		int nzvec = 2*fft_ni*fft_nj;
		double *img_zvec = new double[nzvec];
#endif


#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

		int l;
#ifndef USE_FFTW
		for (i=0; i < nzvec; i++) img_zvec[i] = 0;
#endif
		for (img_index=0; img_index < image_npixels; img_index++)
		{
			i = pixel_map_i[img_index];
			j = pixel_map_j[img_index];
			if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j]))) {
				i -= fft_imin;
				j -= fft_jmin;
#ifdef USE_FFTW
				l = j*fft_ni + i;
				img_rvec[l] = surface_brightness_vector[img_index];
#else
				k = 2*(j*fft_ni + i);
				img_zvec[k] = surface_brightness_vector[img_index];
#endif
			}
		}
#ifdef USE_OPENMP
		if (show_wtime) {
			wtime = omp_get_wtime() - wtime0;
			if (mpi_id==0) {
				cout << "Wall time for setting up PSF convolution via FFT: " << wtime << endl;
			}
		}
#endif
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

#ifdef USE_FFTW
		fftw_execute(fftplan);
		for (i=0; i < ncomplex; i++) {
			img_transform[i] = img_transform[i]*psf_transform[i];
			img_transform[i] /= (fft_ni*fft_nj);
		}
		fftw_execute(fftplan_inverse);
#else
		int nnvec[2];
		nnvec[0] = fft_ni;
		nnvec[1] = fft_nj;
		fourier_transform(img_zvec,2,nnvec,1);

		double rtemp, itemp;
		for (i=0,j=0; i < (fft_ni*fft_nj); i++, j += 2) {
			rtemp = (img_zvec[j]*psf_zvec[j] - img_zvec[j+1]*psf_zvec[j+1]) / (fft_ni*fft_nj);
			itemp = (img_zvec[j]*psf_zvec[j+1] + img_zvec[j+1]*psf_zvec[j]) / (fft_ni*fft_nj);
			img_zvec[j] = rtemp;
			img_zvec[j+1] = itemp;
		}
		fourier_transform(img_zvec,2,nnvec,-1);
#endif

		for (img_index=0; img_index < image_npixels; img_index++)
		{
			i = pixel_map_i[img_index];
			j = pixel_map_j[img_index];
			if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j]))) {
				i -= fft_imin;
				j -= fft_jmin;
#ifdef USE_FFTW
				l = j*fft_ni + i;
				surface_brightness_vector[img_index] = img_rvec[l];
#else
				k = 2*(j*fft_ni + i);
				surface_brightness_vector[img_index] = img_zvec[k];
#endif
			}
		}
#ifdef USE_OPENMP
		if (show_wtime) {
			wtime = omp_get_wtime() - wtime0;
			if (mpi_id==0) {
				cout << "Wall time for calculating PSF convolution via FFT: " << wtime << endl;
			}
		}
#endif
#ifndef USE_FFTW
		delete[] img_zvec;
#endif
	} else {
		int npix;
		int *pixel_map_i, *pixel_map_j;
		int **pix_index;
		double **psf;
		int psf_nx, psf_ny;
		if (foreground) {
			npix = image_npixels_fgmask;
			pixel_map_i = active_image_pixel_i_fgmask;
			pixel_map_j = active_image_pixel_j_fgmask;
			pix_index = image_pixel_grid->pixel_index_fgmask;
			psf = foreground_psf_matrix;
			psf_nx = foreground_psf_npixels_x;
			psf_ny = foreground_psf_npixels_y;
			if (use_input_psf_matrix) {
				if (foreground_psf_matrix == NULL) return;
			} else {
				if ((psf_width_x==0) and (psf_width_y==0)) return;
				else if (generate_PSF_matrix(image_pixel_grid->pixel_xlength,image_pixel_grid->pixel_ylength)==false) {
					if (verbal) warn("could not generate_PSF matrix");
					return;
				}
			}
		} else {
			if (use_input_psf_matrix) {
				if (psf_matrix == NULL) return;
			} else {
				if ((psf_width_x==0) and (psf_width_y==0)) return;
				else if (generate_PSF_matrix(image_pixel_grid->pixel_xlength,image_pixel_grid->pixel_ylength)==false) {
					if (verbal) warn("could not generate_PSF matrix");
					return;
				}
			}
			npix = image_npixels;
			pixel_map_i = active_image_pixel_i;
			pixel_map_j = active_image_pixel_j;
			pix_index = image_pixel_grid->pixel_index;
			psf = psf_matrix;
			psf_nx = psf_npixels_x;
			psf_ny = psf_npixels_y;
		}
		nx_half = psf_nx/2;
		ny_half = psf_ny/2;

		double *new_surface_brightness_vector = new double[npix];
		int i,j,k,l;
		int psf_k, psf_l;
		int img_index1, img_index2;
		double totweight; // we'll use this to keep track of whether the full PSF area is not used (e.g. near borders or near masked pixels), and adjust
		// the weighting if necessary
		#pragma omp parallel for private(k,l,i,j,img_index1,img_index2,psf_k,psf_l,totweight) schedule(static)
		for (img_index1=0; img_index1 < npix; img_index1++)
		{ // this loops over columns of the PSF blurring matrix
			new_surface_brightness_vector[img_index1] = 0;
			totweight = 0;
			k = pixel_map_i[img_index1];
			l = pixel_map_j[img_index1];
			for (psf_k=0; psf_k < psf_nx; psf_k++) {
				i = k + nx_half - psf_k; // Note, 'k' is the index for the convolved image, so we have k = i - nx_half + psf_k
				if ((i >= 0) and (i < image_pixel_grid->x_N)) {
					for (psf_l=0; psf_l < psf_ny; psf_l++) {
						j = l + ny_half - psf_l; // Note, 'l' is the index for the convolved image, so we have l = j - ny_half + psf_l
						if ((j >= 0) and (j < image_pixel_grid->y_N)) {
							// THIS IS VERY CLUMSY! RE-IMPLEMENT IN A MORE ELEGANT WAY?
							if (((foreground) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_data->foreground_mask[i][j]))) or  
							((!foreground) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j])))) {
								img_index2 = pix_index[i][j];
								new_surface_brightness_vector[img_index1] += psf[psf_k][psf_l]*surface_brightness_vector[img_index2];
								totweight += psf[psf_k][psf_l];
								//cout << "PSF: " << psf_k << " " << psf_l << " " << psf[psf_k][psf_l] << endl;
							}
						}
					}
				}
			}
			if (totweight != 1.0) new_surface_brightness_vector[img_index1] /= totweight;
			//cout << "WEIGHT=" << totweight << endl;
		}

#ifdef USE_OPENMP
		if (show_wtime) {
			wtime = omp_get_wtime() - wtime0;
			if (mpi_id==0) {
				if (foreground) cout << "Wall time for calculating PSF convolution of foreground: " << wtime << endl;
				else cout << "Wall time for calculating PSF convolution of image: " << wtime << endl;
			}
		}
#endif

		for (i=0; i < npix; i++) {
			surface_brightness_vector[i] = new_surface_brightness_vector[i];
			//cout << surface_brightness_vector[i] << endl;
		}
		delete[] new_surface_brightness_vector;
	}
}

#define DSWAP(a,b) dtemp=(a);(a)=(b);(b)=dtemp;
void QLens::fourier_transform(double* data, const int ndim, int* nn, const int isign)
{
	int idim,i1,i2,i3,i2rev,i3rev,ip1,ip2,ip3,ifp1,ifp2;
	int ibit,k1,k2,n,nprev,nrem,ntot=1;
	double tempi,tempr,theta,wi,wpi,wpr,wr,wtemp,dtemp;
	for (i1=0; i1 < ndim; i1++) ntot *= nn[i1];

	nprev=1;
	for (idim=ndim-1;idim>=0;idim--) {
		n=nn[idim];
		nrem=ntot/(n*nprev);
		ip1=nprev << 1;
		ip2=ip1*n;
		ip3=ip2*nrem;
		i2rev=0;
		for (i2=0;i2<ip2;i2+=ip1) {
			if (i2 < i2rev) {
				for (i1=i2;i1<i2+ip1-1;i1+=2) {
					for (i3=i1;i3<ip3;i3+=ip2) {
						i3rev=i2rev+i3-i2;
						DSWAP(data[i3],data[i3rev]);
						DSWAP(data[i3+1],data[i3rev+1]);

					}
				}
			}
			ibit=ip2 >> 1;
			while ((ibit >= ip1) and ((i2rev+1) > ibit)) {
				i2rev -= ibit;
				ibit >>= 1;
			}
			i2rev += ibit;
		}
		ifp1=ip1;
		while (ifp1 < ip2) {
			ifp2=ifp1 << 1;
			theta=isign*M_2PI/(ifp2/ip1);
			wtemp=sin(theta/2);
			wpr = -2.0*wtemp*wtemp;
			wpi=sin(theta);
			wr=1.0;
			wi=0.0;
			for (i3=0;i3<ifp1;i3+=ip1) {
				for (i1=i3;i1<i3+ip1-1;i1+=2) {
					for (i2=i1;i2<ip3;i2+=ifp2) {
						k1=i2;
						k2=k1+ifp1;
						tempr=wr*data[k2]-wi*data[k2+1];
						tempi=wr*data[k2+1]+wi*data[k2];
						data[k2]=data[k1]-tempr;
						data[k2+1]=data[k1+1]-tempi;
						data[k1] += tempr;
						data[k1+1] += tempi;
					}
				}
				wr=(wtemp=wr)*wpr-wi*wpi+wr;
				wi=wi*wpr+wtemp*wpi+wi;
			}
			ifp1=ifp2;
		}
		nprev *= n;
	}
}
#undef DSWAP

bool QLens::generate_PSF_matrix(const double xstep, const double ystep)
{
	//static const double sigma_fraction = 1.6; // the bigger you make this, the less sparse the matrix will become (more pixel-pixel correlations)
	if (psf_threshold==0) return false; // need a threshold to determine where to truncate the PSF
	double sigma_fraction = sqrt(-2*log(psf_threshold));
	int i,j;
	int nx_half, ny_half, nx, ny;
	double x, y, xmax, ymax;
	if ((psf_width_x==0) or (psf_width_y==0)) return false;
	double normalization = 0;
	double nx_half_dec, ny_half_dec;
	//xstep = image_pixel_grid->pixel_xlength;
	//ystep = image_pixel_grid->pixel_ylength;
	nx_half_dec = sigma_fraction*psf_width_x/xstep;
	ny_half_dec = sigma_fraction*psf_width_y/ystep;
	nx_half = ((int) nx_half_dec);
	ny_half = ((int) ny_half_dec);
	if ((nx_half_dec - nx_half) > 0.5) nx_half++;
	if ((ny_half_dec - ny_half) > 0.5) ny_half++;
	xmax = nx_half*xstep;
	ymax = ny_half*ystep;
	//cout << "PIXEL LENGTHS: " << xstep << " " << ystep << endl;
	//cout << "nxhalf=" << nx_half << " nyhalf=" << ny_half << " xmax=" << xmax << " ymax=" << ymax << endl;
	nx = 2*nx_half+1;
	ny = 2*ny_half+1;
	if (psf_matrix != NULL) {
		for (i=0; i < psf_npixels_x; i++) delete[] psf_matrix[i];
		delete[] psf_matrix;
	}
	//cout << "NX=" << nx << " NY=" << ny << endl;
	psf_matrix = new double*[nx];
	for (i=0; i < nx; i++) psf_matrix[i] = new double[ny];
	psf_npixels_x = nx;
	psf_npixels_y = ny;
	//cout << " NPIXELS: " << nx << " " << ny << endl;
	//cout << "PSF widths: " << psf_width_x << " " << psf_width_y << endl;
	for (i=0, x=-xmax; i < nx; i++, x += xstep) {
		for (j=0, y=-ymax; j < ny; j++, y += ystep) {
			psf_matrix[i][j] = exp(-0.5*(SQR(x/psf_width_x) + SQR(y/psf_width_y)));
			normalization += psf_matrix[i][j];
			//cout << "creating PSF: " << i << " " << j << " x=" << x << " y=" << y << " " << psf_matrix[i][j] << endl;
		}
	}
	for (i=0; i < nx; i++) {
		for (j=0; j < ny; j++) {
			psf_matrix[i][j] /= normalization;
		}
	}
	setup_foreground_PSF_matrix(); // just sets foreground PSF to be same as PSF for lensed images (maybe later allow them to be different?)
	return true;
}

bool QLens::spline_PSF_matrix(const double xstep, const double ystep)
{
	if (psf_matrix==NULL) return false;
	int i,nx_half,ny_half;
	double x,y;
	nx_half = psf_npixels_x/2;
	ny_half = psf_npixels_y/2;
	double xmax = nx_half*xstep;
	double ymax = ny_half*ystep;
	double *xvals = new double[psf_npixels_x];
	double *yvals = new double[psf_npixels_y];
	for (i=0, x=-xmax; i < psf_npixels_x; i++, x += xstep) xvals[i] = x;
	for (i=0, y=-ymax; i < psf_npixels_y; i++, y += ystep) yvals[i] = y;
	psf_spline.input(xvals,yvals,psf_matrix,psf_npixels_x,psf_npixels_y);
	delete[] xvals;
	delete[] yvals;
	return true;
}

double QLens::interpolate_PSF_matrix(const double x, const double y)
{
	double psfint;
	if (psf_spline.is_splined()) {
		psfint = psf_spline.splint(x,y);
	} else {
		double scaled_x, scaled_y;
		int ii,jj;
		double nx_half, ny_half;
		nx_half = psf_npixels_x/2;
		ny_half = psf_npixels_y/2;
		scaled_x = (x / image_pixel_grid->pixel_xlength) + nx_half;
		scaled_y = (y / image_pixel_grid->pixel_ylength) + ny_half;
		ii = (int) scaled_x;
		jj = (int) scaled_y;
		if ((ii < 0) or (jj < 0) or (ii >= psf_npixels_x-1) or (jj >= psf_npixels_y-1)) return 0.0;
		double tt,TT,uu,UU;
		tt = scaled_x - ii;
		TT = 1-tt;
		uu = scaled_y - jj;
		UU = 1-uu;
		psfint = TT*UU*psf_matrix[ii][jj] + tt*UU*psf_matrix[ii+1][jj] + TT*uu*psf_matrix[ii][jj+1] + tt*uu*psf_matrix[ii+1][jj+1];
	}
	//cout << "PSFINT: " << psfint << endl;
	return psfint;
}

void QLens::create_regularization_matrix()
{
	if (Rmatrix != NULL) delete[] Rmatrix;
	if (Rmatrix_index != NULL) delete[] Rmatrix_index;

	int i,j;

	dense_Rmatrix = false; // assume sparse unless a dense regularization is chosen
	switch (regularization_method) {
		case Norm:
			generate_Rmatrix_norm(); break;
		case Gradient:
			generate_Rmatrix_from_gmatrices(); break;
		case Curvature:
			generate_Rmatrix_from_hmatrices(); break;
		case Exponential_Kernel:
			dense_Rmatrix = true;
			generate_Rmatrix_from_covariance_kernel(true);
			break;
		case Squared_Exponential_Kernel:
			dense_Rmatrix = true;
			generate_Rmatrix_from_covariance_kernel(false);
			break;
		default:
			die("Regularization method not recognized");
	}
	if ((inversion_method==DENSE) or (inversion_method==DENSE_FMATRIX)) {
		// If doing a sparse inversion, the determinant of R-matrix will be calculated when doing the inversion; otherwise, must be done here
#ifdef USE_MKL
		Rmatrix_determinant_MKL();
#else
#ifdef USE_UMFPACK
		Rmatrix_determinant_UMFPACK();
#else
#ifdef USE_MUMPS
		Rmatrix_determinant_MUMPS();
#else
	die("Currently either compiling with MUMPS, UMFPACK, or MKL is required to calculate sparse R-matrix determinants");
#endif
#endif
#endif
	}

	//cout << "Printing Rmatrix..." << endl;
	//int indx;	
	//for (i=0; i < source_npixels; i++) {
		//indx = Rmatrix_index[i];
		//int nn = Rmatrix_index[i+1]-Rmatrix_index[i];
		//cout << "Row " << i << ": " << nn << " entries (starts at index " << indx << ")" << endl;
		//cout << "diag: " << Rmatrix[i] << endl;
		//for (j=0; j < nn; j++) {
			//cout << i << " " << Rmatrix_index[indx+j] << " " << Rmatrix[indx+j] << endl;
		//}
	//}
}

void QLens::create_regularization_matrix_shapelet()
{
	if (source_npixels==0) return;
	dense_Rmatrix = false;
	switch (regularization_method) {
		case Norm:
			generate_Rmatrix_norm(); break;
		case Gradient:
			generate_Rmatrix_shapelet_gradient(); break;
		case Curvature:
			generate_Rmatrix_shapelet_curvature(); break;
		default:
			die("Regularization method not recognized for dense matrices");
	}
#ifdef USE_MKL
		Rmatrix_determinant_MKL();
#else
#ifdef USE_UMFPACK
		Rmatrix_determinant_UMFPACK();
#else
#ifdef USE_MUMPS
		Rmatrix_determinant_MUMPS();
#else
	die("Currently either compiling with MUMPS, UMFPACK, or MKL is required to calculate sparse R-matrix determinants");
#endif
#endif
#endif
}

void QLens::generate_Rmatrix_norm()
{
	Rmatrix_nn = source_npixels+1;
	Rmatrix = new double[Rmatrix_nn];
	Rmatrix_index = new int[Rmatrix_nn];

	for (int i=0; i < source_npixels; i++) {
		Rmatrix[i] = 1;
		Rmatrix_index[i] = source_npixels+1;
	}
	Rmatrix_index[source_npixels] = source_npixels+1;

	Rmatrix_log_determinant = 0;
}

void QLens::generate_Rmatrix_shapelet_gradient()
{
	bool at_least_one_shapelet = false;
	Rmatrix_nn = 3*source_npixels+1; // actually it will be slightly less than this due to truncation at shapelets with i=n_shapelets-1 or j=n_shapelets-1

	Rmatrix = new double[Rmatrix_nn];
	Rmatrix_index = new int[Rmatrix_nn];

	for (int i=0; i < n_sb; i++) {
		if (sb_list[i]->sbtype==SHAPELET) {
			sb_list[i]->calculate_gradient_Rmatrix_elements(Rmatrix, Rmatrix_index);
			at_least_one_shapelet = true;
			break;
		}
	}
	if (!at_least_one_shapelet) die("No shapelet profile has been created; cannot calculate regularization matrix");
	Rmatrix_nn = Rmatrix_index[source_npixels];
	//for (int i=0; i <= source_npixels; i++) cout << Rmatrix[i] << " " << Rmatrix_index[i] << endl;
	//cout << "Rmatrix_nn=" << Rmatrix_nn << " source_npixels=" << source_npixels << endl;
}

void QLens::generate_Rmatrix_shapelet_curvature()
{
	Rmatrix_nn = source_npixels+1;

	Rmatrix = new double[Rmatrix_nn];
	Rmatrix_index = new int[Rmatrix_nn];

	bool at_least_one_shapelet = false;
	for (int i=0; i < n_sb; i++) {
		if (sb_list[i]->sbtype==SHAPELET) {
			sb_list[i]->calculate_curvature_Rmatrix_elements(Rmatrix, Rmatrix_index);
			at_least_one_shapelet = true;
		}
	}
	if (!at_least_one_shapelet) die("No shapelet profile has been created; cannot calculate regularization matrix");
	Rmatrix_nn = Rmatrix_index[source_npixels];
}

void QLens::create_lensing_matrices_from_Lmatrix(const bool dense_Fmatrix, const bool verbal)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*group_comm, *mpi_group, &sub_comm);
#endif

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	double *Fmatrix_stacked; // used only if dense_Fmatrix is set to true
	//double effective_reg_parameter = regularization_parameter * (1000.0/source_n_amps);
	double effective_reg_parameter = regularization_parameter;

	double covariance; // right now we're using a uniform uncorrelated noise for each pixel; will generalize this later
	if (data_pixel_noise==0) covariance = 1; // if there is no noise it doesn't matter what the covariance is, since we won't be regularizing
	else covariance = SQR(data_pixel_noise);

	int i,j,k,l,m,t;

	vector<int> *Fmatrix_index_rows = new vector<int>[source_n_amps];
	vector<double> *Fmatrix_rows = new vector<double>[source_n_amps];
	double *Fmatrix_diags = new double[source_n_amps];
	int *Fmatrix_row_nn = new int[source_n_amps];
	Fmatrix_nn = 0;
	int Fmatrix_nn_part = 0;
	for (j=0; j < source_n_amps; j++) {
		Fmatrix_diags[j] = 0;
		Fmatrix_row_nn[j] = 0;
	}
	int ntot = source_n_amps*(source_n_amps+1)/2;

	bool new_entry;
	int src_index1, src_index2, col_index, col_i;
	double tmp, element;
	Dvector = new double[source_n_amps];
	for (i=0; i < source_n_amps; i++) Dvector[i] = 0;

	int pix_i, pix_j, img_index_fgmask;
	double sbcov;
	for (i=0; i < image_npixels; i++) {
		pix_i = active_image_pixel_i[i];
		pix_j = active_image_pixel_j[i];
		img_index_fgmask = image_pixel_grid->pixel_index_fgmask[pix_i][pix_j];
		sbcov = image_surface_brightness[i] - sbprofile_surface_brightness[img_index_fgmask];
		if (((vary_srcflux) and (!include_imgfluxes_in_inversion)) and (n_sourcepts_fit > 0)) sbcov -= point_image_surface_brightness[i];
		sbcov /= covariance;
		for (j=image_pixel_location_Lmatrix[i]; j < image_pixel_location_Lmatrix[i+1]; j++) {
			//Dvector[Lmatrix_index[j]] += Lmatrix[j]*(image_surface_brightness[i] - sbprofile_surface_brightness[i])/covariance;
			//Dvector[Lmatrix_index[j]] += Lmatrix[j]*(image_surface_brightness[i] - image_pixel_grid->foreground_surface_brightness[pix_i][pix_j])/covariance;
			Dvector[Lmatrix_index[j]] += Lmatrix[j]*sbcov;
		}
	}

	int mpi_chunk, mpi_start, mpi_end;
	mpi_chunk = source_n_amps / group_np;
	mpi_start = group_id*mpi_chunk;
	if (group_id == group_np-1) mpi_chunk += (source_n_amps % group_np); // assign the remainder elements to the last mpi process
	mpi_end = mpi_start + mpi_chunk;

#ifdef USE_MKL
	int *srcpixel_location_Fmatrix, *srcpixel_end_Fmatrix, *Fmatrix_csr_index;
	double *Fmatrix_csr;
	int nsrc1, nsrc2;
	sparse_index_base_t indxing;
	sparse_matrix_t Lsparse;
	sparse_matrix_t Fsparse;
	int *image_pixel_end_Lmatrix = new int[image_npixels];
	for (i=0; i < image_npixels; i++) image_pixel_end_Lmatrix[i] = image_pixel_location_Lmatrix[i+1];
	//cout << "Creating CSR matrix..." << endl;
	mkl_sparse_d_create_csr(&Lsparse, SPARSE_INDEX_BASE_ZERO, image_npixels, source_n_amps, image_pixel_location_Lmatrix, image_pixel_end_Lmatrix, Lmatrix_index, Lmatrix);
	mkl_sparse_order(Lsparse);
	sparse_status_t status;
	if (!dense_Fmatrix) {
		status = mkl_sparse_syrk(SPARSE_OPERATION_TRANSPOSE, Lsparse, &Fsparse);
		mkl_sparse_d_export_csr(Fsparse, &indxing, &nsrc1, &nsrc2, &srcpixel_location_Fmatrix, &srcpixel_end_Fmatrix, &Fmatrix_csr_index, &Fmatrix_csr);

		if ((verbal) and (mpi_id==0)) cout << "Fmatrix_sparse has " << srcpixel_end_Fmatrix[source_n_amps-1] << " elements" << endl;
		bool duplicate_column;
		int dup_k;
		for (i=0; i < source_n_amps; i++) {
			for (j=srcpixel_location_Fmatrix[i]; j < srcpixel_end_Fmatrix[i]; j++) {
				duplicate_column = false;
				if (Fmatrix_csr_index[j]==i) {
					Fmatrix_diags[i] += Fmatrix_csr[j]/covariance;
					//cout << "Adding " << Fmatrix_csr[j] << " to diag " << i << endl;
				}
				else if (Fmatrix_csr[j] != 0) {
					for (k=0; k < Fmatrix_index_rows[i].size(); k++) if (Fmatrix_csr_index[j]==Fmatrix_index_rows[i][k]) {
						duplicate_column = true;
						dup_k = k;
					}
					if (duplicate_column) {
						Fmatrix_rows[i][k] += Fmatrix_csr[j]/covariance;
						die("duplicate!"); // this is not a big deal, but if duplicates never happen, then you might want to redo this part so it allocates memory in one go for each row instead of a bunch of push_back's
					} else {
						Fmatrix_rows[i].push_back(Fmatrix_csr[j]/covariance);
						Fmatrix_index_rows[i].push_back(Fmatrix_csr_index[j]);
						Fmatrix_row_nn[i]++;
						Fmatrix_nn_part++;
					}
				}
			}
		}
		//cout << "Done!" << endl;
		//cout << "LMATRIX:" << endl;
		//for (i=0; i < image_npixels; i++) {
			//cout << "Row " << i << ":" << endl;
			//for (j=image_pixel_location_Lmatrix[i]; j < image_pixel_location_Lmatrix[i+1]; j++) {
				//cout << Lmatrix_index[j] << " " << Lmatrix[j] << endl;
			//}
		//}

		//cout << endl << "FMATRIX:" << endl;

		//for (i=0; i < source_n_amps; i++) {
			//cout << "Row " << i << ":" << endl;
			//for (j=srcpixel_location_Fmatrix[i]; j < srcpixel_end_Fmatrix[i]; j++) {
				//cout << Fmatrix_csr_index[j] << " " << Fmatrix_csr[j] << endl;
			//}
		//}
	} else {
		Fmatrix_packed.input(ntot);
		Fmatrix_stacked = new double[source_n_amps*source_n_amps];
		for (i=0; i < source_n_amps*source_n_amps; i++) Fmatrix_stacked[i] = 0;
		mkl_sparse_d_syrkd (SPARSE_OPERATION_TRANSPOSE, Lsparse, 1.0, 0.0, Fmatrix_stacked, SPARSE_LAYOUT_ROW_MAJOR, source_n_amps);
		LAPACKE_dtrttp(LAPACK_ROW_MAJOR,'U',source_n_amps,Fmatrix_stacked,source_n_amps,Fmatrix_packed.array());
		int nf=0;
		for (i=0; i < ntot; i++) {
			if (Fmatrix_packed[i] != 0) {
				Fmatrix_packed[i] /= covariance;
				nf++;
			}
		}
		if ((verbal) and (mpi_id==0)) cout << "Fmatrix_dense has " << nf << " nonzero elements" << endl;
	}
#else
	vector<jl_pair> **jlvals = new vector<jl_pair>*[nthreads];
	for (i=0; i < nthreads; i++) {
		jlvals[i] = new vector<jl_pair>[source_n_amps];
	}

	jl_pair jl;
	#pragma omp parallel
	{
		int thread;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
	// idea: just store j and l, so that all the calculating can be done in the loop below (which can be made parallel much more easily)
		#pragma omp for private(i,j,l,jl,src_index1,src_index2,tmp) schedule(dynamic)
		for (i=0; i < image_npixels; i++) {
			for (j=image_pixel_location_Lmatrix[i]; j < image_pixel_location_Lmatrix[i+1]; j++) {
				for (l=j; l < image_pixel_location_Lmatrix[i+1]; l++) {
					src_index1 = Lmatrix_index[j];
					src_index2 = Lmatrix_index[l];
					if (src_index1 > src_index2) {
						jl.l=j; jl.j=l;
						jlvals[thread][src_index2].push_back(jl);
					} else {
						jl.j=j; jl.l=l;
						jlvals[thread][src_index1].push_back(jl);
					}
				}
			}
		}

#ifdef USE_OPENMP
		#pragma omp barrier
		#pragma omp master
		{
			if (show_wtime) {
				wtime = omp_get_wtime() - wtime0;
				if (mpi_id==0) cout << "Wall time for calculating Fmatrix (storing jvals,lvals): " << wtime << endl;
				wtime0 = omp_get_wtime();
			}
		}
#endif

		#pragma omp for private(i,j,k,l,m,t,src_index1,src_index2,new_entry,col_index,col_i,element) schedule(static) reduction(+:Fmatrix_nn_part)
		for (src_index1=mpi_start; src_index1 < mpi_end; src_index1++) {
			col_i=0;
			for (t=0; t < nthreads; t++) {
				for (k=0; k < jlvals[t][src_index1].size(); k++) {
					j = jlvals[t][src_index1][k].j;
					l = jlvals[t][src_index1][k].l;
					src_index2 = Lmatrix_index[l];
					new_entry = true;
					element = Lmatrix[j]*Lmatrix[l]/covariance; // generalize this to full covariance matrix later
					if (src_index1==src_index2) Fmatrix_diags[src_index1] += element;
					else {
						m=0;
						while ((m < Fmatrix_row_nn[src_index1]) and (new_entry==true))
						{
							if (Fmatrix_index_rows[src_index1][m]==src_index2) {
								new_entry = false;
								col_index = m;
							}
							m++;
						}
						if (new_entry) {
							Fmatrix_rows[src_index1].push_back(element);
							Fmatrix_index_rows[src_index1].push_back(src_index2);
							Fmatrix_row_nn[src_index1]++;
							col_i++;
						}
						else Fmatrix_rows[src_index1][col_index] += element;
					}
				}
			}
			Fmatrix_nn_part += col_i;

			/*
			if (regularization_method != None) {
				if (!optimize_regparam) Fmatrix_diags[src_index1] += effective_reg_parameter*Rmatrix[src_index1];
				col_i=0;
				for (j=Rmatrix_index[src_index1]; j < Rmatrix_index[src_index1+1]; j++) {
					new_entry = true;
					k=0;
					while ((k < Fmatrix_row_nn[src_index1]) and (new_entry==true)) {
						if (Rmatrix_index[j]==Fmatrix_index_rows[src_index1][k]) {
							new_entry = false;
							col_index = k;
						}
						k++;
					}
					if (new_entry) {
						if (!optimize_regparam) {
							Fmatrix_rows[src_index1].push_back(effective_reg_parameter*Rmatrix[j]);
						} else {
							Fmatrix_rows[src_index1].push_back(0);
							// This way, when we're optimizing the regularization parameter, the needed entries are already there to add to
						}
						Fmatrix_index_rows[src_index1].push_back(Rmatrix_index[j]);
						Fmatrix_row_nn[src_index1]++;
						col_i++;
					} else {
						if (!optimize_regparam) {
							Fmatrix_rows[src_index1][col_index] += effective_reg_parameter*Rmatrix[j];
						}
					}
				}
				Fmatrix_nn_part += col_i;
			}
		*/
		}
	}
#endif

	if (!dense_Fmatrix) {
		if (regularization_method != None) {
			for (src_index1=mpi_start; src_index1 < mpi_end; src_index1++) {
				if (src_index1 < source_npixels) { // additional source amplitudes are not regularized
					if (!optimize_regparam) Fmatrix_diags[src_index1] += effective_reg_parameter*Rmatrix[src_index1];
					col_i=0;
					for (j=Rmatrix_index[src_index1]; j < Rmatrix_index[src_index1+1]; j++) {
						new_entry = true;
						k=0;
						while ((k < Fmatrix_row_nn[src_index1]) and (new_entry==true)) {
							if (Rmatrix_index[j]==Fmatrix_index_rows[src_index1][k]) {
								new_entry = false;
								col_index = k;
							}
							k++;
						}
						if (new_entry) {
							if (!optimize_regparam) {
							//cout << "Fmat row " << src_index1 << ", col " << (Rmatrix_index[j]) << ": was 0, now adding " << (effective_reg_parameter*Rmatrix[j]) << endl;
								Fmatrix_rows[src_index1].push_back(effective_reg_parameter*Rmatrix[j]);
							} else {
								Fmatrix_rows[src_index1].push_back(0);
								// This way, when we're optimizing the regularization parameter, the needed entries are already there to add to
							}
							Fmatrix_index_rows[src_index1].push_back(Rmatrix_index[j]);
							Fmatrix_row_nn[src_index1]++;
							col_i++;
						} else {
							if (!optimize_regparam) {
							//cout << "Fmat row " << src_index1 << ", col " << (Rmatrix_index[j]) << ": was " << Fmatrix_rows[src_index1][col_index] << ", now adding " << (effective_reg_parameter*Rmatrix[j]) << endl;
								Fmatrix_rows[src_index1][col_index] += effective_reg_parameter*Rmatrix[j];
							}

						}
					}
					Fmatrix_nn_part += col_i;
				}
			}
		}

#ifdef USE_MPI
		MPI_Allreduce(&Fmatrix_nn_part, &Fmatrix_nn, 1, MPI_INT, MPI_SUM, sub_comm);
#else
		Fmatrix_nn = Fmatrix_nn_part;
#endif
		Fmatrix_nn += source_n_amps+1;

		Fmatrix = new double[Fmatrix_nn];
		Fmatrix_index = new int[Fmatrix_nn];

#ifdef USE_MPI
		int id, chunk, start, end, length;
		for (id=0; id < group_np; id++) {
			chunk = source_n_amps / group_np;
			start = id*chunk;
			if (id == group_np-1) chunk += (source_n_amps % group_np); // assign the remainder elements to the last mpi process
			MPI_Bcast(Fmatrix_row_nn + start,chunk,MPI_INT,id,sub_comm);
			MPI_Bcast(Fmatrix_diags + start,chunk,MPI_DOUBLE,id,sub_comm);
		}
#endif

		Fmatrix_index[0] = source_n_amps+1;
		for (i=0; i < source_n_amps; i++) {
			Fmatrix_index[i+1] = Fmatrix_index[i] + Fmatrix_row_nn[i];
		}
		if (Fmatrix_index[source_n_amps] != Fmatrix_nn) die("Fmatrix # of elements don't match up (%i vs %i), process %i",Fmatrix_index[source_n_amps],Fmatrix_nn,mpi_id);

		for (i=0; i < source_n_amps; i++)
			Fmatrix[i] = Fmatrix_diags[i];

		int indx;
		for (i=mpi_start; i < mpi_end; i++) {
			indx = Fmatrix_index[i];
			for (j=0; j < Fmatrix_row_nn[i]; j++) {
				Fmatrix[indx+j] = Fmatrix_rows[i][j];
				Fmatrix_index[indx+j] = Fmatrix_index_rows[i][j];
			}
		}

#ifdef USE_MPI
		for (id=0; id < group_np; id++) {
			chunk = source_n_amps / group_np;
			start = id*chunk;
			if (id == group_np-1) chunk += (source_n_amps % group_np); // assign the remainder elements to the last mpi process
			end = start + chunk;
			length = Fmatrix_index[end] - Fmatrix_index[start];
			MPI_Bcast(Fmatrix + Fmatrix_index[start],length,MPI_DOUBLE,id,sub_comm);
			MPI_Bcast(Fmatrix_index + Fmatrix_index[start],length,MPI_INT,id,sub_comm);
		}
		MPI_Comm_free(&sub_comm);
#endif

#ifdef USE_OPENMP
		if (show_wtime) {
			wtime = omp_get_wtime() - wtime0;
			if (mpi_id==0) cout << "Wall time for Fmatrix MPI communication + construction: " << wtime << endl;
		}
#endif
		if ((mpi_id==0) and (verbal)) cout << "Fmatrix now has " << Fmatrix_nn << " elements\n";

		if ((mpi_id==0) and (verbal)) {
			int Fmatrix_ntot = source_n_amps*(source_n_amps+1)/2;
			double sparseness = ((double) Fmatrix_nn)/Fmatrix_ntot;
			cout << "src_npixels = " << source_n_amps << endl;
			cout << "Fmatrix ntot = " << Fmatrix_ntot << endl;
			cout << "Fmatrix sparseness = " << sparseness << endl;
		}
	} else {
		if (dense_Rmatrix) {
			if ((regularization_method != None) and (!optimize_regparam)) {
				int n_extra_amps = source_n_amps - source_npixels;
				double *Fptr, *Rptr;
				Fptr = Fmatrix_packed.array();
				Rptr = Rmatrix_packed.array();
				for (i=0; i < source_npixels; i++) {
					for (j=i; j < source_npixels; j++) {
						*(Fptr++) += effective_reg_parameter*(*(Rptr++));
					}
					Fptr += n_extra_amps;
				}
				//for (i=0; i < ntot; i++) Fmatrix_packed[i] += effective_reg_parameter*Rmatrix_packed[i];
			}
		} else {
			int k,indx_start=0;
			if ((regularization_method != None) and (!optimize_regparam)) {
				for (i=0; i < source_npixels; i++) {
					Fmatrix_packed[indx_start] += effective_reg_parameter*Rmatrix[i];
					for (k=Rmatrix_index[i]; k < Rmatrix_index[i+1]; k++) {
						//cout << "Fmat row " << i << ", col " << (Rmatrix_index[k]) << ": was " << Fmatrix_packed[indx_start+Rmatrix_index[k]-i] << ", now adding " << (effective_reg_parameter*Rmatrix[k]) << endl;
						Fmatrix_packed[indx_start+Rmatrix_index[k]-i] += effective_reg_parameter*Rmatrix[k];
					}
					indx_start += source_n_amps-i;
				}
			}
		}
	}

#ifdef USE_OPENMP
		if (show_wtime) {
			wtime = omp_get_wtime() - wtime0;
			if (mpi_id==0) cout << "Wall time for calculating Fmatrix elements: " << wtime << endl;
			wtime0 = omp_get_wtime();
		}
#endif

	//cout << "FMATRIX (SPARSE):" << endl;
	//for (i=0; i < source_n_amps; i++) {
		//cout << i << "," << i << ": " << Fmatrix[i] << endl;
		//for (j=Fmatrix_index[i]; j < Fmatrix_index[i+1]; j++) {
			//cout << i << "," << Fmatrix_index[j] << ": " << Fmatrix[j] << endl;
		//}
		//cout << endl;
	//}

/*
	bool found;
	cout << "LMATRIX:" << endl;
	for (i=0; i < image_npixels; i++) {
		for (j=0; j < source_n_amps; j++) {
			found = false;
			for (k=image_pixel_location_Lmatrix[i]; k < image_pixel_location_Lmatrix[i+1]; k++) {
				if (Lmatrix_index[k]==j) {
					found = true;
					cout << Lmatrix[k] << " ";
				}
			}
			if (!found) cout << "0 ";
		}
		cout << endl;
	}
	*/	

#ifdef USE_MKL
	mkl_sparse_destroy(Lsparse);
	if (!dense_Fmatrix) mkl_sparse_destroy(Fsparse);
	else delete[] Fmatrix_stacked;
	delete[] image_pixel_end_Lmatrix;
#else
	for (i=0; i < nthreads; i++) {
		delete[] jlvals[i];
	}
	delete[] jlvals;
#endif
	delete[] Fmatrix_index_rows;
	delete[] Fmatrix_rows;
	delete[] Fmatrix_diags;
	delete[] Fmatrix_row_nn;
}

void QLens::create_lensing_matrices_from_Lmatrix_dense(const bool verbal)
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	//double effective_reg_parameter = regularization_parameter * (1000.0/source_n_amps);
	double effective_reg_parameter = regularization_parameter;

	double covariance; // right now we're using a uniform uncorrelated noise for each pixel; will generalize this later
	if (data_pixel_noise==0) covariance = 1; // if there is no noise it doesn't matter what the covariance is, since we won't be regularizing
	else covariance = SQR(data_pixel_noise);

	int i,j,l,n;

	bool new_entry;
	Dvector = new double[source_n_amps];
	for (i=0; i < source_n_amps; i++) Dvector[i] = 0;

	int ntot = source_n_amps*(source_n_amps+1)/2;
	Fmatrix_packed.input(ntot);

#ifdef USE_MKL
   double *Ltrans_stacked = new double[source_n_amps*image_npixels];
   double *Fmatrix_stacked = new double[source_n_amps*source_n_amps];
#else
	double *i_n = new double[ntot];
	double *j_n = new double[ntot];
	double **Ltrans = new double*[source_n_amps];
	//int **ncheck = new int*[source_n_amps];
	n=0;
	for (i=0; i < source_n_amps; i++) {
		Ltrans[i] = new double[image_npixels];
		//ncheck[j] = new int[source_n_amps];
		for (j=i; j < source_n_amps; j++) {
			//ncheck[i][j] = n;
			i_n[n] = i;
			j_n[n] = j;
			n++;
		}
	}
#endif

	#pragma omp parallel
	{
		int thread;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
		int row;
		int pix_i, pix_j;
		int img_index_fgmask;
		double sb_adj;
		#pragma omp for private(i,j,pix_i,pix_j,img_index_fgmask,row,sb_adj) schedule(static)
		for (i=0; i < source_n_amps; i++) {
			row = i*image_npixels;
			for (j=0; j < image_npixels; j++) {
				pix_i = active_image_pixel_i[j];
				pix_j = active_image_pixel_j[j];
				img_index_fgmask = image_pixel_grid->pixel_index_fgmask[pix_i][pix_j];
				//Dvector[i] += Lmatrix_dense[j][i]*(image_surface_brightness[j] - sbprofile_surface_brightness[j])/covariance;
				//Dvector[i] += Lmatrix_dense[j][i]*(image_surface_brightness[j] - image_pixel_grid->foreground_surface_brightness[pix_i][pix_j])/covariance;
				if ((zero_sb_extended_mask_prior) and (include_extended_mask_in_inversion) and (image_pixel_data->extended_mask[pix_i][pix_j]) and (!image_pixel_data->in_mask[pix_i][pix_j])) ; 
				else {
					sb_adj = image_surface_brightness[j] - sbprofile_surface_brightness[img_index_fgmask];
					if (((vary_srcflux) and (!include_imgfluxes_in_inversion)) and (n_sourcepts_fit > 0)) sb_adj -= point_image_surface_brightness[j];
					Dvector[i] += Lmatrix_dense[j][i]*sb_adj/covariance;
					if (sbprofile_surface_brightness[img_index_fgmask]*0.0 != 0.0) die("FUCK");
				}
#ifdef USE_MKL
				Ltrans_stacked[row+j] = Lmatrix_dense[j][i]/sqrt(covariance); // hack to get the covariance in there
#else
				Ltrans[i][j] = Lmatrix_dense[j][i];
#endif
			}
		}

#ifdef USE_OPENMP
		#pragma omp master
		{
			if (show_wtime) {
				wtime = omp_get_wtime() - wtime0;
				if (mpi_id==0) cout << "Wall time for initializing Fmatrix and Dvector: " << wtime << endl;
				wtime0 = omp_get_wtime();
			}
		}
#endif

#ifndef USE_MKL
		// The following is not as fast as the Blas function dsyrk (below), but it still gets the job done
		double *fpmatptr;
		double *lmatptr1, *lmatptr2;
		#pragma omp for private(n,i,j,l,lmatptr1,lmatptr2,fpmatptr) schedule(static)
		for (n=0; n < ntot; n++) {
			i = i_n[n];
			j = j_n[n];
			fpmatptr = Fmatrix_packed.array()+n;
			lmatptr1 = Ltrans[i];
			lmatptr2 = Ltrans[j];
			(*fpmatptr) = 0;
			for (l=0; l < image_npixels; l++) {
				(*fpmatptr) += (*(lmatptr1++))*(*(lmatptr2++));
			}
			(*fpmatptr) /= covariance;
		}
#endif
	}

#ifdef USE_MKL
   cblas_dsyrk(CblasRowMajor,CblasUpper,CblasNoTrans,source_n_amps,image_npixels,1,Ltrans_stacked,image_npixels,0,Fmatrix_stacked,source_n_amps);
   LAPACKE_dtrttp(LAPACK_ROW_MAJOR,'U',source_n_amps,Fmatrix_stacked,source_n_amps,Fmatrix_packed.array());
#endif

   int k,indx_start=0;
   if ((regularization_method != None) and (!optimize_regparam)) {
      for (i=0; i < source_npixels; i++) { // additional source amplitudes (beyond source_npixels) are not regularized
         Fmatrix_packed[indx_start] += effective_reg_parameter*Rmatrix[i];
			for (k=Rmatrix_index[i]; k < Rmatrix_index[i+1]; k++) {
				//cout << "Fmat row " << i << ", col " << (Rmatrix_index[k]) << ": was " << Fmatrix_packed[indx_start+Rmatrix_index[k]-i] << ", now adding " << (effective_reg_parameter*Rmatrix[k]) << endl;
				Fmatrix_packed[indx_start+Rmatrix_index[k]-i] += effective_reg_parameter*Rmatrix[k];
			}
			indx_start += source_n_amps-i;
      }
   }
	//double Ftot = 0;
	//for (i=0; i < ntot; i++) Ftot += Fmatrix_packed[i];
	//double ltot = 0;
	//for (i=0; i < source_n_amps; i++) {
		//for (j=0; j < image_npixels; j++) {
			//ltot += Lmatrix_dense[i][j];
		//}
	//}
	//cout << "Ltot, Ftot: " << ltot << " " << Ftot << endl;

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating Fmatrix dense elements: " << wtime << endl;
		wtime0 = omp_get_wtime();
	}
#endif
#ifdef USE_MKL
	delete[] Ltrans_stacked;
	delete[] Fmatrix_stacked;
#else
	for (i=0; i < source_n_amps; i++) delete[] Ltrans[i];
	delete[] Ltrans;
	delete[] i_n;
	delete[] j_n;
#endif
}

void QLens::optimize_regularization_parameter(const bool dense_Fmatrix, const bool verbal)
{
#ifdef USE_OPENMP
	double wtime_opt0, wtime_opt;
	if (show_wtime) {
		wtime_opt0 = omp_get_wtime();
	}
#endif
	img_minus_sbprofile = new double[image_npixels];
	int i, pix_i, pix_j, img_index_fgmask;
	for (i=0; i < image_npixels; i++) {
		pix_i = active_image_pixel_i[i];
		pix_j = active_image_pixel_j[i];
		img_index_fgmask = image_pixel_grid->pixel_index_fgmask[pix_i][pix_j];
		img_minus_sbprofile[i] = image_surface_brightness[i] - sbprofile_surface_brightness[img_index_fgmask];
		if (((vary_srcflux) and (!include_imgfluxes_in_inversion)) and (n_sourcepts_fit > 0)) img_minus_sbprofile[i] -= point_image_surface_brightness[i];
	}

	double logreg, logrmin = 0, logrmax = 3;
	if (dense_Fmatrix) {
		int ntot = source_n_amps*(source_n_amps+1)/2;
		//Fmatrix_copy.input(source_n_amps,source_n_amps);
		Fmatrix_packed_copy.input(ntot);
	} else {
		//Fmatrix_nn = Fmatrix_index[source_n_amps];
		if (Fmatrix_nn==0) die("Fmatrix length has not been set");
		Fmatrix_copy = new double[Fmatrix_nn];
	}
	temp_src.input(source_n_amps);

	double (QLens::*chisqreg)(const double);
	if (dense_Fmatrix) chisqreg = &QLens::chisq_regparam_dense;
	else chisqreg = &QLens::chisq_regparam;
	double logreg_min = brents_min_method(chisqreg,optimize_regparam_minlog,optimize_regparam_maxlog,optimize_regparam_tol,verbal);
	//(this->*chisqreg)(log(regularization_parameter)/ln10);
	regularization_parameter = pow(10,logreg_min);
	if ((verbal) and (mpi_id==0)) cout << "regparam after optimizing: " << regularization_parameter << endl;

   int j,k,indx_start=0;

	if (dense_Fmatrix) {
		if (dense_Rmatrix) {
			int n_extra_amps = source_n_amps - source_npixels;
			double *Fptr, *Rptr;
			Fptr = Fmatrix_packed.array();
			Rptr = Rmatrix_packed.array();
			for (i=0; i < source_npixels; i++) {
				for (j=i; j < source_npixels; j++) {
					*(Fptr++) += regularization_parameter*(*(Rptr++));
				}
				Fptr += n_extra_amps;
			}
		} else {
			for (i=0; i < source_npixels; i++) {
				Fmatrix_packed[indx_start] += regularization_parameter*Rmatrix[i];
				for (k=Rmatrix_index[i]; k < Rmatrix_index[i+1]; k++) {
					Fmatrix_packed[indx_start+Rmatrix_index[k]-i] += regularization_parameter*Rmatrix[k];
				}
				indx_start += source_n_amps-i;
			}
		}
	} else {
		for (i=0; i < source_npixels; i++) {
			Fmatrix[i] += regularization_parameter*Rmatrix[i];
			for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
				for (k=Fmatrix_index[i]; k < Fmatrix_index[i+1]; k++) {
					if (Rmatrix_index[j]==Fmatrix_index[k]) {
						Fmatrix[k] += regularization_parameter*Rmatrix[j];
						break;
					}
				}
			}
		}
	}
	if (!dense_Fmatrix) {
		delete[] Fmatrix_copy;
		Fmatrix_copy = NULL;
	}

	delete[] img_minus_sbprofile;
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime_opt = omp_get_wtime() - wtime_opt0;
		if (mpi_id==0) cout << "Wall time for optimizing regularization parameter: " << wtime_opt << endl;
		wtime_opt0 = omp_get_wtime();
	}
#endif
}

double QLens::chisq_regparam(const double logreg)
{
	double covariance; // right now we're using a uniform uncorrelated noise for each pixel; will generalize this later
	if (data_pixel_noise==0) covariance = 1; // if there is no noise it doesn't matter what the covariance is, since we won't be regularizing
	else covariance = SQR(data_pixel_noise);

	regularization_parameter = pow(10,logreg);
	int i,j,k;

	for (i=0; i < Fmatrix_nn; i++) {
		Fmatrix_copy[i] = Fmatrix[i];
	}

	for (i=0; i < source_npixels; i++) {
		Fmatrix_copy[i] += regularization_parameter*Rmatrix[i];
		for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
			for (k=Fmatrix_index[i]; k < Fmatrix_index[i+1]; k++) {
				if (Rmatrix_index[j]==Fmatrix_index[k]) {
					Fmatrix_copy[k] += regularization_parameter*Rmatrix[j];
				}
			}
		}
	}

	double Fmatrix_logdet;

	if (inversion_method==MUMPS) invert_lens_mapping_MUMPS(false,true);
	else if (inversion_method==UMFPACK) invert_lens_mapping_UMFPACK(false,true);
	else die("can only use MUMPS or UMFPACK for sparse inversions with optimize_regparam on");

	double temp_img, Ed_times_two=0,Es_times_two=0;
	double *Lmatptr;
	//double *tempsrcptr = temp_src.array();

	//ofstream wtfout("wtfpix.dat");
	//for (i=0; i < image_npixels; i++) {
		//wtfout << active_image_pixel_i[i] << " " << active_image_pixel_j[i] << " " << image_surface_brightness[i] << " " << sbprofile_surface_brightness[i] << " " << (image_surface_brightness[i] - sbprofile_surface_brightness[i]) << endl;
	//}
	//die();
	//ofstream srcout("tempsrc.dat");
	//for (i=0; i < source_n_amps; i++) {
		//srcout << source_pixel_vector[i] << endl;
	//}

	int pix_i, pix_j, img_index_fgmask;
	#pragma omp parallel for private(temp_img,i,j,pix_i,pix_j,img_index_fgmask,Lmatptr) schedule(static) reduction(+:Ed_times_two)
	for (i=0; i < image_npixels; i++) {
		//pix_i = active_image_pixel_i[i];
		//pix_j = active_image_pixel_j[i];
		//img_index_fgmask = image_pixel_grid->pixel_index_fgmask[pix_i][pix_j];
		temp_img = 0;
		//Lmatptr = (Lmatrix_dense.pointer())[i];
		//tempsrcptr = temp_src.array();
		//for (j=0; j < source_n_amps; j++) {
			//temp_img += (*(Lmatptr++))*(*(tempsrcptr++));
		//}
		for (j=image_pixel_location_Lmatrix[i]; j < image_pixel_location_Lmatrix[i+1]; j++) {
			temp_img += Lmatrix[j]*source_pixel_vector[Lmatrix_index[j]];
		}
		//if (image_surface_brightness[i] < 0) image_surface_brightness[i] = 0;


		//Ed_times_two += SQR(temp_img -  image_surface_brightness[i] + sbprofile_surface_brightness[i])/covariance;
		//Ed_times_two += SQR(temp_img -  image_surface_brightness[i] + image_pixel_grid->foreground_surface_brightness[pix_i][pix_j])/covariance;
		//Ed_times_two += SQR(temp_img -  image_surface_brightness[i] + sbprofile_surface_brightness[img_index_fgmask])/covariance;
		// NOTE: this chisq does not include foreground mask pixels that lie outside the primary mask, since those pixels don't contribute to determining the regularization
		Ed_times_two += SQR(temp_img - img_minus_sbprofile[i])/covariance;
		//cout << "TEMPIMG: " << temp_img << " " << img_minus_sbprofile[i] << endl;
	}
	for (i=0; i < source_npixels; i++) {
		Es_times_two += Rmatrix[i]*SQR(source_pixel_vector[i]);
		for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
			Es_times_two += 2 * source_pixel_vector[i] * Rmatrix[j] * source_pixel_vector[Rmatrix_index[j]]; // factor of 2 since matrix is symmetric
		}
	}
	//cout << "regparam: "<< regularization_parameter << endl;
	//cout << "chisqreg: " << (Ed_times_two + regularization_parameter*Es_times_two + Fmatrix_log_determinant - source_n_amps*log(regularization_parameter) - Rmatrix_log_determinant) << endl;
	//cout << "reg*Es_times_two=" << (regularization_parameter*Es_times_two) << " n_shapelets*log(regparam)=" << (-source_n_amps*log(regularization_parameter)) << " -det(Rmatrix)=" << (-Rmatrix_log_determinant) << " log(Fmatrix)=" << Fmatrix_logdet << endl;

	return (Ed_times_two + regularization_parameter*Es_times_two + Fmatrix_log_determinant - source_npixels*log(regularization_parameter) - Rmatrix_log_determinant);
}

double QLens::chisq_regparam_dense(const double logreg)
{
	double covariance; // right now we're using a uniform uncorrelated noise for each pixel; will generalize this later
	if (data_pixel_noise==0) covariance = 1; // if there is no noise it doesn't matter what the covariance is, since we won't be regularizing
	else covariance = SQR(data_pixel_noise);

	regularization_parameter = pow(10,logreg);
	int i,j;

	if (dense_Rmatrix) {
		for (i=0; i < Fmatrix_packed.size(); i++) {
			Fmatrix_packed_copy[i] = Fmatrix_packed[i];
		}
		int n_extra_amps = source_n_amps - source_npixels;
		double *Fptr, *Rptr;
		Fptr = Fmatrix_packed_copy.array();
		Rptr = Rmatrix_packed.array();
		for (i=0; i < source_npixels; i++) {
			for (j=i; j < source_npixels; j++) {
				*(Fptr++) += regularization_parameter*(*(Rptr++));
			}
			Fptr += n_extra_amps;
		}
	} else {
		for (i=0; i < Fmatrix_packed.size(); i++) {
			Fmatrix_packed_copy[i] = Fmatrix_packed[i];
		}

		int k,indx_start=0;
		for (i=0; i < source_npixels; i++) {
			Fmatrix_packed_copy[indx_start] += regularization_parameter*Rmatrix[i];
			for (k=Rmatrix_index[i]; k < Rmatrix_index[i+1]; k++) {
				Fmatrix_packed_copy[indx_start+Rmatrix_index[k]-i] += regularization_parameter*Rmatrix[k];
			}
			indx_start += source_n_amps-i;
		}
	}
	double Fmatrix_logdet;
#ifdef USE_MKL
   LAPACKE_dpptrf(LAPACK_ROW_MAJOR,'U',source_n_amps,Fmatrix_packed_copy.array());
	for (int i=0; i < source_n_amps; i++) temp_src[i] = Dvector[i];
	LAPACKE_dpptrs(LAPACK_ROW_MAJOR,'U',source_n_amps,1,Fmatrix_packed_copy.array(),temp_src.array(),1);
	Cholesky_logdet_packed(Fmatrix_packed_copy.array(),Fmatrix_logdet,source_n_amps);
#else
	// At the moment, the native (non-MKL) Cholesky decomposition code does a lower triangular decomposition; since Fmatrix/Rmatrix stores the upper
	// triangular part, we have to switch Fmatrix to a lower triangular version here. Fix later so it uses the upper triangular Cholesky version!!!
	repack_Fmatrix_lower();

	bool status = Cholesky_dcmp_packed(Fmatrix_packed_copy.array(),Fmatrix_logdet,source_n_amps);
	if (!status) die("Cholesky decomposition failed");
	Cholesky_solve_lower_packed(Fmatrix_packed_copy.array(),Dvector,temp_src.array(),source_n_amps);
	Cholesky_logdet_lower_packed(Fmatrix_packed_copy.array(),Fmatrix_logdet,source_n_amps);
#endif

	double temp_img, Ed_times_two=0,Es_times_two=0;
	double *Lmatptr;
	double *tempsrcptr = temp_src.array();
	double *tempsrc_end = temp_src.array() + source_n_amps;

	//ofstream wtfout("wtfpix.dat");
	//for (i=0; i < image_npixels; i++) {
		//wtfout << active_image_pixel_i[i] << " " << active_image_pixel_j[i] << " " << image_surface_brightness[i] << " " << sbprofile_surface_brightness[i] << " " << (image_surface_brightness[i] - sbprofile_surface_brightness[i]) << endl;
	//}
	//die();

	int pix_i, pix_j, img_index_fgmask;
	#pragma omp parallel for private(temp_img,i,j,pix_i,pix_j,img_index_fgmask,Lmatptr,tempsrcptr) schedule(static) reduction(+:Ed_times_two)
	for (i=0; i < image_npixels; i++) {
		//pix_i = active_image_pixel_i[i];
		//pix_j = active_image_pixel_j[i];
		//img_index_fgmask = image_pixel_grid->pixel_index_fgmask[pix_i][pix_j];
		temp_img = 0;
		if ((source_fit_mode==Shapelet_Source) or (inversion_method==DENSE)) {
			// even if using a pixellated source, if inversion_method is set to DENSE, only the dense form of the Lmatrix has been convolved with the PSF, so this form must be used
			Lmatptr = (Lmatrix_dense.pointer())[i];
			tempsrcptr = temp_src.array();
			while (tempsrcptr != tempsrc_end) {
				temp_img += (*(Lmatptr++))*(*(tempsrcptr++));
			}
		} else {
			for (j=image_pixel_location_Lmatrix[i]; j < image_pixel_location_Lmatrix[i+1]; j++) {
				temp_img += Lmatrix[j]*temp_src[Lmatrix_index[j]];
			}
		}
		//Ed_times_two += SQR(temp_img -  image_surface_brightness[i] + sbprofile_surface_brightness[i])/covariance;
		//Ed_times_two += SQR(temp_img -  image_surface_brightness[i] + image_pixel_grid->foreground_surface_brightness[pix_i][pix_j])/covariance;
		//Ed_times_two += SQR(temp_img -  image_surface_brightness[i] + sbprofile_surface_brightness[img_index_fgmask])/covariance;
		// NOTE: this chisq does not include foreground mask pixels that lie outside the primary mask, since those pixels don't contribute to determining the regularization
		Ed_times_two += SQR(temp_img - img_minus_sbprofile[i])/covariance;
	}
	if (dense_Rmatrix) {
		double *Rptr, *sptr_i, *sptr_j, *s_end;
		Rptr = Rmatrix_packed.array();
		s_end = temp_src.array() + source_npixels;
		for (sptr_i=temp_src.array(); sptr_i != s_end; sptr_i++) {
			sptr_j = sptr_i;
			Es_times_two += (*sptr_i)*(*(Rptr++))*(*sptr_j++);
			while (sptr_j != s_end) {
				Es_times_two += 2*(*sptr_i)*(*(Rptr++))*(*(sptr_j++)); // factor of 2 since matrix is symmetric
			}
		}
	} else {
		for (i=0; i < source_npixels; i++) {
			Es_times_two += Rmatrix[i]*SQR(temp_src[i]);
			for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
				Es_times_two += 2 * temp_src[i] * Rmatrix[j] * temp_src[Rmatrix_index[j]]; // factor of 2 since matrix is symmetric
			}
		}
	}
	//cout << "regparam: "<< regularization_parameter << endl;
	//cout << "chisqreg: " << (Ed_times_two + regularization_parameter*Es_times_two + Fmatrix_logdet - source_n_amps*log(regularization_parameter) - Rmatrix_log_determinant) << endl;
	//cout << "reg*Es_times_two=" << (regularization_parameter*Es_times_two) << " n_shapelets*log(regparam)=" << (-source_n_amps*log(regularization_parameter)) << " -det(Rmatrix)=" << (-Rmatrix_log_determinant) << " log(Fmatrix)=" << Fmatrix_logdet << endl;

	return (Ed_times_two + regularization_parameter*Es_times_two + Fmatrix_logdet - source_npixels*log(regularization_parameter) - Rmatrix_log_determinant);
}

double QLens::brents_min_method(double (QLens::*func)(const double), const double ax, const double bx, const double tol, const bool verbal)
{
	double a,b,d=0.0,etemp,fu,fv,fw,fx;
	double p,q,r,tol1,tol2,u,v,w,x,xm;
	double e=0.0;

	const int ITMAX = 100;
	const double CGOLD = 0.3819660;
	const double ZEPS = 1.0e-10;

	a = ax;
	b = ax;
	x=w=v=bx;
	fw=fv=fx=(this->*func)(x);
	for (int iter=0; iter < ITMAX; iter++)
	{
		xm=0.5*(a+b);
		tol2 = 2.0 * ((tol1=tol*abs(x)) + ZEPS);
		if (abs(x-xm) <= (tol2-0.5*(b-a))) {
			if ((verbal) and (mpi_id==0)) cout << "Number of regparam optimizing iterations: " << iter << endl;
			return x;
		}
		if (abs(e) > tol1) {
			r = (x-w)*(fx-fv);
			q = (x-v)*(fx-fw);
			p = (x-v)*q - (x-w)*r;
			q = 2.0*(q-r);
			if (q > 0.0) p = -p;
			q = abs(q);
			etemp = e;
			e = d;
			if (abs(p) >= abs(0.5*q*etemp) or p <= q*(a-x) or p >= q*(b-x))
				d = CGOLD*(e=(x >= xm ? a-x : b-x));
			else {
				d = p/q;
				u = x + d;
				if (u-a < tol2 or b-u < tol2)
					d = brent_sign(tol1,xm-x);
			}
		} else {
			d = CGOLD*(e=(x >= xm ? a-x : b-x));
		}
		u = (abs(d) >= tol1 ? x+d : x + brent_sign(tol1,d));
		fu = (this->*func)(u);
		if (fu <= fx) {
			if (u >= x) a=x; else b=x;
			//shft3(v,w,x,u);
			v=w; w=x; x=u;
			//shft3(fv,fw,fx,fu);
			fv = fw; fw = fx; fx=fu;
		} else {
			if (u < x) a=u; else b=u;
			if (fu <= fw or w == x) {
				v = w;
				w = u;
				fv = fw;
				fw = fu;
			} else if (fu <= fv or v == x or v == w) {
				v = u;
				fv = fu;
			}
		}
	}
	warn("Brent's Method reached maximum number of iterations for optimizing regparam");
	return x;
}

/*
bool QLens::Cholesky_dcmp(double** a, double &logdet, int n)
{
	int i,j,k;

	logdet = log(abs(a[0][0]));
	a[0][0] = sqrt(a[0][0]);
	for (j=1; j < n; j++) a[j][0] /= a[0][0];

	bool status = true;
	for (i=1; i < n; i++) {
		//#pragma omp parallel for private(j,k) schedule(static)
		for (j=i; j < n; j++) {
			for (k=0; k < i; k++) {
				a[j][i] -= a[i][k]*a[j][k];
			}
		}
		if (a[i][i] < 0) {
			warn("matrix is not positive-definite (row %i)",i);
			status = false;
		}
		logdet += log(abs(a[i][i]));
		a[i][i] = sqrt(abs(a[i][i]));
		for (j=i+1; j < n; j++) a[j][i] /= a[i][i];
	}
	// switch to upper triangular (annoying, shouldn't have to!)
	for (i=0; i < n; i++) {
		for (j=0; j < i; j++) {
			a[j][i] = a[i][j];
			a[i][j] = 0;
		}
	}
	
	return status;
}
*/

/*
// Not sure why this upper version doesn't work...trying to start from bottom-right and go upwards from there
bool QLens::Cholesky_dcmp_upper(double** a, double &logdet, int n)
{
	int i,j,k;

	logdet = log(abs(a[n-1][n-1]));
	a[n-1][n-1] = sqrt(a[n-1][n-1]);
	for (j=0; j < n-1; j++) a[j][n-1] /= a[n-1][n-1];

	bool status = true;
	for (i=n-2; i >= 0; i--) {
		//#pragma omp parallel for private(j,k) schedule(static)
		for (j=i; j >= 0; j--) {
			for (k=n-1; k >= i; k--) {
				a[j][i] -= a[i][k]*a[j][k];
			}
		}
		if (a[i][i] < 0) {
			warn("matrix is not positive-definite (row %i)",i);
			status = false;
		}
		logdet += log(abs(a[i][i]));
		a[i][i] = sqrt(abs(a[i][i]));
		for (j=0; j < i; j++) a[j][i] /= a[i][i];
	}
	
	return status;
}
*/

/*
bool QLens::Cholesky_dcmp_upper(double** a, double &logdet, int n)
{
	int i,j,k;

	logdet = log(abs(a[0][0]));
	a[0][0] = sqrt(a[0][0]);
	for (j=1; j < n; j++) a[0][j] /= a[0][0];

	bool status = true;
	for (i=1; i < n; i++) {
		#pragma omp parallel for private(j,k) schedule(static)
		for (j=i; j < n; j++) {
			for (k=0; k < i; k++) {
				a[i][j] -= a[k][i]*a[k][j];
			}
		}
		if (a[i][i] < 0) {
			warn("matrix is not positive-definite (row %i)",i);
			status = false;
		}
		logdet += log(abs(a[i][i]));
		a[i][i] = sqrt(abs(a[i][i]));
		for (j=i+1; j < n; j++) a[i][j] /= a[i][i];
	}
	
	return status;
}
*/

/*
bool QLens::Cholesky_dcmp_upper_packed(double* a, double &logdet, int n)
{
	int i,j,k;

	int *indx = new int[n];
	indx[0] = 0;
	for (j=0; j < n; j++) if (j > 0) indx[j] = indx[j-1] + n-j;

	a[0] = sqrt(a[0]);
	for (j=1; j < n; j++) a[j] /= a[0];

	bool status = true;
	double *aptr1, *aptr2, *aptr3;
	for (i=1; i < n; i++) {
		#pragma omp parallel for private(j,k,aptr1,aptr2,aptr3) schedule(static)
		for (j=i; j < n; j++) {
			aptr1 = a+indx[i];
			aptr2 = a+indx[j];
			aptr3 = aptr2+i;
			for (k=0; k < i; k++) {
				*(aptr3) -= (*(aptr1++))*(*(aptr2++));
			}
		}
		aptr1 = a+indx[i]+i;
		if ((*aptr1) < 0) {
			warn("matrix is not positive-definite (row %i)",i);
			status = false;
		}
		(*aptr1) = sqrt(abs((*aptr1)));
		for (j=0; j < i; j++) a[indx[j]+i-j] /= (*aptr1);
	}
	delete[] indx;
	
	return status;
}
*/

// This does a lower triangular Cholesky decomposition
bool QLens::Cholesky_dcmp_packed(double* a, double &logdet, int n)
{
	int i,j,k;

	int *indx = new int[n];
	indx[0] = 0;
	for (j=0; j < n; j++) if (j > 0) indx[j] = indx[j-1] + j;

	a[0] = sqrt(a[0]);
	for (j=1; j < n; j++) a[indx[j]] /= a[0];

	bool status = true;
	double *aptr1, *aptr2, *aptr3;
	for (i=1; i < n; i++) {
		#pragma omp parallel for private(j,k,aptr1,aptr2,aptr3) schedule(static)
		for (j=i; j < n; j++) {
			aptr1 = a+indx[i];
			aptr2 = a+indx[j];
			aptr3 = aptr2+i;
			for (k=0; k < i; k++) {
				*(aptr3) -= (*(aptr1++))*(*(aptr2++));
			}
		}
		aptr1 = a+indx[i]+i;
		if ((*aptr1) < 0) {
			warn("matrix is not positive-definite (row %i)",i);
			status = false;
		}
		(*aptr1) = sqrt(abs((*aptr1)));
		for (j=i+1; j < n; j++) a[indx[j]+i] /= (*aptr1);
	}
	delete[] indx;
	
	return status;
}

/*
void QLens::Cholesky_invert_lower(double** a, const int n)
{
	double sum;
	int i,j,k;
	for (i=0; i < n; i++) {
		a[i][i] = 1.0/a[i][i];
		for (j=i+1; j < n; j++) {
			sum=0.0;
			for (k=i; k < j; k++) sum -= a[j][k]*a[k][i];
			a[j][i]=sum/a[j][j];
		}
	}
}
*/

/*
void QLens::Cholesky_invert_upper(double** a, const int n)
{
	double sum;
	int i,j,k;
	for (i=0; i < n; i++) {
		a[i][i] = 1.0/a[i][i];
		for (j=i+1; j < n; j++) {
			sum=0.0;
			for (k=i; k < j; k++) sum -= a[k][j]*a[i][k];
			a[i][j]=sum/a[j][j];
		}
	}
}
*/

void QLens::Cholesky_invert_upper_packed(double* a, const int n)
{
	double sum;
	int i,j,k;
	int indx=0, indx2;
	// Replace indx, indx2 with pointers, as in upper_triangular_syrk
	for (i=0; i < n; i++) {
		a[indx] = 1.0/a[indx];
		for (j=i+1; j < n; j++) {
			sum=0.0;
			indx2=indx;
			for (k=i; k < j; k++) {
				sum -= a[indx2+j-k]*a[indx+k-i];
				indx2 += n-k;
			}
			a[indx+j-i]=sum/a[indx2];
		}
		indx += n-i;
	}
}

/*
void QLens::upper_triangular_syrk(double* a, const int n)
{
	double sum;
	int i,j,k;
	int indx=0, indx2;
	for (i=0; i < n; i++) {
		indx2=indx;
		for (j=i; j < n; j++) {
			sum=0.0;
			for (k=j; k < n; k++) {
				sum += a[indx+k-i]*a[indx2+k-j];
			}
			a[indx+j-i] = sum;
			indx2 += n-j;
		}
		indx += n-i;
	}
}
*/	

void QLens::upper_triangular_syrk(double* a, const int n)
{
	double sum;
	int i,j,k;
	double *aptr, *aptr2;
	aptr=aptr2=a;
	for (i=0; i < n; i++) {
		aptr2 = aptr;
		for (j=i; j < n; j++) {
			sum=0.0;
			for (k=j; k < n; k++) {
				sum += (*(aptr++))*(*(aptr2++));
			}
			*a = sum;
			aptr = ++a;
		}
	}
}

/*
void QLens::test_inverts()
{
	int i,j;
	dmatrix cov(4,4);
	dmatrix cov2(4,4);
	cov[0][0] = 10;
	cov[0][1] = 2;
	cov[0][2] = 4;
	cov[0][3] = 1;
	cov[1][1] = 10;
	cov[1][2] = 5;
	cov[1][3] = 2;
	cov[2][2] = 10;
	cov[2][3] = 6;
	cov[3][3] = 10;
	for (i=0; i < 4; i++) {
		for (j=0; j < i; j++) {
			cov[i][j] = cov[j][i];
		}
	}
	for (i=0; i < 4; i++) {
		for (j=0; j < 4; j++) {
			cov2[i][j] = cov[i][j];
		}
	}
	cout << "cov:" << endl;
	for (i=0; i < 4; i++) {
		for (j=0; j < 4; j++) {
			cout << cov[i][j] << " ";
		}
		cout << endl;
	}
	cout << endl;

	double logdet,logdet2;
	Cholesky_dcmp(cov.pointer(),logdet,4);
	cout << "decomp1:" << endl;
	for (i=0; i < 4; i++) {
		for (j=0; j < 4; j++) {
			cout << cov[i][j] << " ";
		}
		cout << endl;
	}

	Cholesky_dcmp_upper(cov2.pointer(),logdet2,4);
	cout << endl;
	cout << "decomp2:" << endl;
	for (i=0; i < 4; i++) {
		for (j=0; j < 4; j++) {
			cout << cov2[i][j] << " ";
		}
		cout << endl;
	}
	cout << endl;

}
*/

// This is for the determinant from the lower triangular version of the decomposition
void QLens::Cholesky_logdet_lower_packed(double* a, double &logdet, int n)
{
	logdet = 0;
	int indx = 0;
	for (int i=0; i < n; i++) {
		logdet += log(abs(a[indx]));
		indx += i+2;
	}
	logdet *= 2;
}

/*
// This function is kept for reference so you can convert to an upper triangular version (not done yet)...after that you can get rid of this
void QLens::Cholesky_solve(double** a, double* b, double* x, int n)
{
	int i,k;
	double sum;
	for (i=0; i < n; i++) {
		for (sum=b[i], k=i-1; k >= 0; k--) sum -= a[i][k]*x[k];
		x[i] = sum / a[i][i];
	}
	for (i=n-1; i >= 0; i--) {
		for (sum=x[i], k=i+1; k < n; k++) sum -= a[k][i]*x[k];
		x[i] = sum / a[i][i];
	}	 
}
*/

// This is the lower triangular version
void QLens::Cholesky_solve_lower_packed(double* a, double* b, double* x, int n)
{
	int i,k;
	double sum;
	int *indx = new int[n];
	indx[0] = 0;
	for (i=0; i < n; i++) {
		if (i > 0) indx[i] = indx[i-1] + i;
		for (sum=b[i], k=i-1; k >= 0; k--) sum -= a[indx[i]+k]*x[k];
		x[i] = sum / a[indx[i]+i];
	}
	for (i=n-1; i >= 0; i--) {
		for (sum=x[i], k=i+1; k < n; k++) sum -= a[indx[k]+i]*x[k];
		x[i] = sum / a[indx[i]+i];
	}	 
	delete[] indx;
}

// This is for the determinant from the upper triangular version of the decomposition
void QLens::Cholesky_logdet_packed(double* a, double &logdet, int n)
{
	logdet = 0;
	int indx = 0;
	for (int i=0; i < n; i++) {
		logdet += log(abs(a[indx]));
		indx += n-i;
	}
	logdet *= 2;
}

/*
// This is an attempt at an upper triangular version of Cholesky solve (not working yet), but you need to make the Cholesky decomp upper triangular as well...fix later
void QLens::Cholesky_solve_packed(double* a, double* b, double* x, int n)
{
	int i,k;
	double sum;
	int *indx = new int[n];
	cout << "HI0" << endl;
	indx[n-1] = (n*(n+1))/2 - 1;
	cout << "HI1" << endl;
	for (i=n-1; i >= 0; i--) {
		if (i < n-1) indx[i] = indx[i+1] - n + i;
		for (sum=b[i], k=1; k < n-1-i; k++) sum -= a[indx[i]+k]*x[k+i];
		x[i] = sum / a[indx[i]];
		cout << "Setting y[" << i << "]" << endl;
	}
	cout << "HI2" << endl;
	for (i=0; i < n; i++) {
		for (sum=x[i], k=i-1; k >= 0; k--) sum -= a[indx[k]+i-k]*x[k];
		x[i] = sum / a[indx[i]];
		cout << "Setting x[" << i << "]" << endl;
	}	 
	cout << "HI3" << endl;
	delete[] indx;
}
*/

void QLens::repack_Fmatrix_lower()
{
	// At the moment, the native Cholesky decomposition code does a lower triangular decomposition; since Fmatrix/Rmatrix stores the upper triangular part,
	// we have to switch Fmatrix to a lower triangular version here
	double **Fmat = new double*[source_n_amps];
	int i,j,k;
	for (i=0; i < source_n_amps; i++) {
		Fmat[i] = new double[i+1];
	}
	for (k=0,j=0; j < source_n_amps; j++) {
		for (i=j; i < source_n_amps; i++) {
			Fmat[i][j] = Fmatrix_packed[k++];
		}
	}
	for (k=0,i=0; i < source_n_amps; i++) {
		for (j=0; j <= i; j++) {
			Fmatrix_packed[k++] = Fmat[i][j];
		}
		delete[] Fmat[i];
	}
	delete[] Fmat;
}

void QLens::repack_Fmatrix_upper()
{
	// At the moment, the native Cholesky decomposition code does a lower triangular decomposition; since Fmatrix/Rmatrix stores the upper triangular part,
	// we have to switch Fmatrix to a lower triangular version here
	double **Fmat = new double*[source_n_amps];
	int i,j,k;
	for (i=0; i < source_n_amps; i++) {
		Fmat[i] = new double[i+1];
	}
	for (k=0,i=0; i < source_n_amps; i++) {
		for (j=0; j <= i; j++) {
			Fmat[i][j] = Fmatrix_packed[k++];
		}
	}
	for (k=0,j=0; j < source_n_amps; j++) {
		for (i=j; i < source_n_amps; i++) {
			Fmatrix_packed[k++] = Fmat[i][j];
		}
	}
	for (i=0; i < source_n_amps; i++) delete[] Fmat[i];
	delete[] Fmat;
}

void QLens::invert_lens_mapping_dense(bool verbal)
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int i,j;
#ifdef USE_MKL
   LAPACKE_dpptrf(LAPACK_ROW_MAJOR,'U',source_n_amps,Fmatrix_packed.array());
	for (int i=0; i < source_n_amps; i++) source_pixel_vector[i] = Dvector[i];
	LAPACKE_dpptrs(LAPACK_ROW_MAJOR,'U',source_n_amps,1,Fmatrix_packed.array(),source_pixel_vector,1);
	Cholesky_logdet_packed(Fmatrix_packed.array(),Fmatrix_log_determinant,source_n_amps);
#else
	// At the moment, the native Cholesky decomposition code does a lower triangular decomposition; since Fmatrix/Rmatrix stores the upper triangular part,
	// we have to switch Fmatrix to a lower triangular version here
	repack_Fmatrix_lower();

	bool status = Cholesky_dcmp_packed(Fmatrix_packed.array(),Fmatrix_log_determinant,source_n_amps);
	if (!status) die("Cholesky decomposition failed");
	Cholesky_solve_lower_packed(Fmatrix_packed.array(),Dvector,source_pixel_vector,source_n_amps);
	Cholesky_logdet_lower_packed(Fmatrix_packed.array(),Fmatrix_log_determinant,source_n_amps);
#endif
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for inverting Fmatrix: " << wtime << endl;
		wtime0 = omp_get_wtime();
	}
#endif

	int index=0;
	if (source_fit_mode==Delaunay_Source) delaunay_srcgrid->update_surface_brightness(index);
	else if (source_fit_mode==Cartesian_Source) source_pixel_grid->update_surface_brightness(index);
	else if (source_fit_mode==Shapelet_Source) {
		double* srcpix = source_pixel_vector;
		for (i=0; i < n_sb; i++) {
			if (sb_list[i]->sbtype==SHAPELET) {
				sb_list[i]->update_amplitudes(srcpix);
			}
		}
	}
	if (include_imgfluxes_in_inversion) {
		index = source_npixels;
		for (j=0; j < point_imgs.size(); j++) {
			for (i=0; i < point_imgs[j].size(); i++) {
				point_imgs[j][i].flux = source_pixel_vector[index++];
			}
		}
	}
}

void QLens::invert_lens_mapping_CG_method(bool verbal)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*group_comm, *mpi_group, &sub_comm);
#endif

#ifdef USE_MPI
	MPI_Barrier(sub_comm);
#endif

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int i,j,k;
	double *temp = new double[source_n_amps];
	// it would be prettier to just pass the MPI communicator in, and have CG_sparse figure out the rank and # of processes internally--implement this later
	CG_sparse cg_method(Fmatrix,Fmatrix_index,1e-4,100000,inversion_nthreads,group_np,group_id);
#ifdef USE_MPI
	cg_method.set_MPI_comm(&sub_comm);
#endif
	for (int i=0; i < source_n_amps; i++) temp[i] = 0;
	if (regularization_method != None)
		cg_method.set_determinant_mode(true);
	else cg_method.set_determinant_mode(false);
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for setting up CG method: " << wtime << endl;
		wtime0 = omp_get_wtime();
	}
#endif
	cg_method.solve(Dvector,temp);

	if ((n_image_prior) or (outside_sb_prior)) {
		max_pixel_sb=-1e30;
		int max_sb_i;
		for (int i=0; i < source_n_amps; i++) {
			if ((data_pixel_noise==0) and (temp[i] < 0)) temp[i] = 0; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			source_pixel_vector[i] = temp[i];
			if (source_pixel_vector[i] > max_pixel_sb) {
				max_pixel_sb = source_pixel_vector[i];
				max_sb_i = i;
			}
		}
		if ((n_image_prior) and (source_fit_mode==Cartesian_Source)) {
			n_images_at_sbmax = source_pixel_n_images[max_sb_i];
			pixel_avg_n_image = 0;
			double sbtot = 0;
			for (int i=0; i < source_n_amps; i++) {
				if (source_pixel_vector[i] >= max_pixel_sb*n_image_prior_sb_frac) {
					pixel_avg_n_image += source_pixel_n_images[i]*source_pixel_vector[i];
					sbtot += source_pixel_vector[i];
				}
			}
			if (sbtot != 0) pixel_avg_n_image /= sbtot;
		}
	} else {
		for (int i=0; i < source_n_amps; i++) {
			if ((data_pixel_noise==0) and (temp[i] < 0)) temp[i] = 0; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			source_pixel_vector[i] = temp[i];
		}
	}

	if (regularization_method != None) {
		cg_method.get_log_determinant(Fmatrix_log_determinant);
		if ((mpi_id==0) and (verbal)) cout << "log determinant = " << Fmatrix_log_determinant << endl;
		CG_sparse cg_det(Rmatrix,Rmatrix_index,3e-4,100000,inversion_nthreads,group_np,group_id);
#ifdef USE_MPI
		cg_det.set_MPI_comm(&sub_comm);
#endif
		Rmatrix_log_determinant = cg_det.calculate_log_determinant();
		if ((mpi_id==0) and (verbal)) cout << "Rmatrix log determinant = " << Rmatrix_log_determinant << endl;
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for inverting Fmatrix: " << wtime << endl;
	}
#endif

	int iterations;
	double error;
	cg_method.get_error(iterations,error);
	if ((mpi_id==0) and (verbal)) cout << iterations << " iterations, error=" << error << endl << endl;

	delete[] temp;
	int index=0;
	if (source_fit_mode==Delaunay_Source) delaunay_srcgrid->update_surface_brightness(index);
	else source_pixel_grid->update_surface_brightness(index);
	if (include_imgfluxes_in_inversion) {
		index = source_npixels;
		for (j=0; j < point_imgs.size(); j++) {
			for (i=0; i < point_imgs[j].size(); i++) {
				point_imgs[j][i].flux = source_pixel_vector[index++];
			}
		}
	}
#ifdef USE_MPI
	MPI_Comm_free(&sub_comm);
#endif
}

void QLens::invert_lens_mapping_UMFPACK(bool verbal, bool use_copy)
{
#ifndef USE_UMFPACK
	die("QLens requires compilation with UMFPACK for factorization");
#else
	bool calculate_determinant = false;
	int default_nthreads=1;

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int i,j;
	double *Fmatptr = (use_copy==true) ? Fmatrix_copy : Fmatrix;

   double *null = (double *) NULL ;
	double *temp = new double[source_n_amps];
   void *Symbolic, *Numeric ;
	double Control [UMFPACK_CONTROL];
	double Info [UMFPACK_INFO];
    umfpack_di_defaults (Control) ;
	 Control[UMFPACK_STRATEGY] = UMFPACK_STRATEGY_SYMMETRIC;

	int Fmatrix_nonzero_elements = Fmatrix_index[source_n_amps]-1;
	int Fmatrix_offdiags = Fmatrix_index[source_n_amps]-1-source_n_amps;
	int Fmatrix_unsymmetric_nonzero_elements = source_n_amps + 2*Fmatrix_offdiags;
	if (Fmatrix_nonzero_elements==0) {
		cout << "nsource_pixels=" << source_n_amps << endl;
		die("Fmatrix has zero size");
	}

	// Now we construct the transpose of Fmatrix so we can cast it into "unsymmetric" format for UMFPACK (by including offdiagonals on either side of diagonal elements)

	double *Fmatrix_transpose = new double[Fmatrix_nonzero_elements+1];
	int *Fmatrix_transpose_index = new int[Fmatrix_nonzero_elements+1];

	int k,jl,jm,jp,ju,m,n2,noff,inc,iv;
	double v;

	n2=Fmatrix_index[0];
	for (j=0; j < n2-1; j++) Fmatrix_transpose[j] = Fmatptr[j];
	int n_offdiag = Fmatrix_index[n2-1] - Fmatrix_index[0];
	int *offdiag_indx = new int[n_offdiag];
	int *offdiag_indx_transpose = new int[n_offdiag];
	for (i=0; i < n_offdiag; i++) offdiag_indx[i] = Fmatrix_index[n2+i];
	indexx(offdiag_indx,offdiag_indx_transpose,n_offdiag);
	for (j=n2, k=0; j < Fmatrix_index[n2-1]; j++, k++) {
		Fmatrix_transpose_index[j] = offdiag_indx_transpose[k];
	}
	jp=0;
	for (k=Fmatrix_index[0]; k < Fmatrix_index[n2-1]; k++) {
		m = Fmatrix_transpose_index[k] + n2;
		Fmatrix_transpose[k] = Fmatptr[m];
		for (j=jp; j < Fmatrix_index[m]+1; j++)
			Fmatrix_transpose_index[j]=k;
		jp = Fmatrix_index[m] + 1;
		jl=0;
		ju=n2-1;
		while (ju-jl > 1) {
			jm = (ju+jl)/2;
			if (Fmatrix_index[jm] > m) ju=jm; else jl=jm;
		}
		Fmatrix_transpose_index[k]=jl;
	}
	for (j=jp; j < n2; j++) Fmatrix_transpose_index[j] = Fmatrix_index[n2-1];
	for (j=0; j < n2-1; j++) {
		jl = Fmatrix_transpose_index[j+1] - Fmatrix_transpose_index[j];
		noff=Fmatrix_transpose_index[j];
		inc=1;
		do {
			inc *= 3;
			inc++;
		} while (inc <= jl);
		do {
			inc /= 3;
			for (k=noff+inc; k < noff+jl; k++) {
				iv = Fmatrix_transpose_index[k];
				v = Fmatrix_transpose[k];
				m=k;
				while (Fmatrix_transpose_index[m-inc] > iv) {
					Fmatrix_transpose_index[m] = Fmatrix_transpose_index[m-inc];
					Fmatrix_transpose[m] = Fmatrix_transpose[m-inc];
					m -= inc;
					if (m-noff+1 <= inc) break;
				}
				Fmatrix_transpose_index[m] = iv;
				Fmatrix_transpose[m] = v;
			}
		} while (inc > 1);
	}
	delete[] offdiag_indx;
	delete[] offdiag_indx_transpose;

	int *Fmatrix_unsymmetric_cols = new int[source_n_amps+1];
	int *Fmatrix_unsymmetric_indices = new int[Fmatrix_unsymmetric_nonzero_elements];
	double *Fmatrix_unsymmetric = new double[Fmatrix_unsymmetric_nonzero_elements];

	int indx=0;
	Fmatrix_unsymmetric_cols[0] = 0;
	for (i=0; i < source_n_amps; i++) {
		for (j=Fmatrix_transpose_index[i]; j < Fmatrix_transpose_index[i+1]; j++) {
			Fmatrix_unsymmetric[indx] = Fmatrix_transpose[j];
			Fmatrix_unsymmetric_indices[indx] = Fmatrix_transpose_index[j];
			indx++;
		}
		Fmatrix_unsymmetric_indices[indx] = i;
		Fmatrix_unsymmetric[indx] = Fmatptr[i];
		indx++;
		for (j=Fmatrix_index[i]; j < Fmatrix_index[i+1]; j++) {
			Fmatrix_unsymmetric[indx] = Fmatptr[j];
			Fmatrix_unsymmetric_indices[indx] = Fmatrix_index[j];
			indx++;
		}
		Fmatrix_unsymmetric_cols[i+1] = indx;
	}

	//cout << "Dvector: " << endl;
	//for (i=0; i < source_n_amps; i++) {
		//cout << Dvector[i] << " ";
	//}
	//cout << endl;

	for (i=0; i < source_n_amps; i++) {
		sort(Fmatrix_unsymmetric_cols[i+1]-Fmatrix_unsymmetric_cols[i],Fmatrix_unsymmetric_indices+Fmatrix_unsymmetric_cols[i],Fmatrix_unsymmetric+Fmatrix_unsymmetric_cols[i]);
		//cout << "Row " << i << ": " << endl;
		//cout << Fmatrix_unsymmetric_cols[i] << " ";
		//for (j=Fmatrix_unsymmetric_cols[i]; j < Fmatrix_unsymmetric_cols[i+1]; j++) {
			//cout << Fmatrix_unsymmetric_indices[j] << " ";
		//}
		//cout << endl;
		//for (j=Fmatrix_unsymmetric_cols[i]; j < Fmatrix_unsymmetric_cols[i+1]; j++) {
			//cout << "j=" << j << " " << Fmatrix_unsymmetric[j] << " ";
		//}
		//cout << endl;
	}
	//cout << endl;

	if (indx != Fmatrix_unsymmetric_nonzero_elements) die("WTF! Wrong number of nonzero elements");

	int status;
   status = umfpack_di_symbolic(source_n_amps, source_n_amps, Fmatrix_unsymmetric_cols, Fmatrix_unsymmetric_indices, Fmatrix_unsymmetric, &Symbolic, Control, Info);
	if (status < 0) {
		umfpack_di_report_info (Control, Info) ;
		umfpack_di_report_status (Control, status) ;
		die("Error inputting matrix");
	}
   status = umfpack_di_numeric(Fmatrix_unsymmetric_cols, Fmatrix_unsymmetric_indices, Fmatrix_unsymmetric, Symbolic, &Numeric, Control, Info);
   umfpack_di_free_symbolic(&Symbolic);

   status = umfpack_di_solve(UMFPACK_A, Fmatrix_unsymmetric_cols, Fmatrix_unsymmetric_indices, Fmatrix_unsymmetric, temp, Dvector, Numeric, Control, Info);

	if (regularization_method != None) calculate_determinant = true; // specifies to calculate determinant

	if ((n_image_prior) or (outside_sb_prior)) {
		max_pixel_sb=-1e30;
		int max_sb_i;
		for (int i=0; i < source_n_amps; i++) {
			source_pixel_vector[i] = temp[i];
			if (source_pixel_vector[i] > max_pixel_sb) {
				max_pixel_sb = source_pixel_vector[i];
				max_sb_i = i;
			}
		}
		if ((n_image_prior) and (source_fit_mode==Cartesian_Source)) {
			n_images_at_sbmax = source_pixel_n_images[max_sb_i];
			pixel_avg_n_image = 0;
			double sbtot = 0;
			for (int i=0; i < source_n_amps; i++) {
				if (source_pixel_vector[i] >= max_pixel_sb*n_image_prior_sb_frac) {
					pixel_avg_n_image += source_pixel_n_images[i]*source_pixel_vector[i];
					sbtot += source_pixel_vector[i];
				}
			}
			if (sbtot != 0) pixel_avg_n_image /= sbtot;
		}
	} else {
		for (int i=0; i < source_n_amps; i++) {
			source_pixel_vector[i] = temp[i];
		}
	}

	double mantissa, exponent;
	status = umfpack_di_get_determinant (&mantissa, &exponent, Numeric, Info) ;
	if (status < 0) {
		die("Could not get determinant using UMFPACK");
	}
	umfpack_di_free_numeric(&Numeric);
	Fmatrix_log_determinant = log(mantissa) + exponent*log(10);

	if (calculate_determinant) Rmatrix_determinant_UMFPACK();
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for inverting Fmatrix: " << wtime << endl;
	}
#endif

	delete[] temp;
	delete[] Fmatrix_transpose;
	delete[] Fmatrix_transpose_index;
	delete[] Fmatrix_unsymmetric_cols;
	delete[] Fmatrix_unsymmetric_indices;
	delete[] Fmatrix_unsymmetric;
	int index=0;
	if (source_fit_mode==Delaunay_Source) delaunay_srcgrid->update_surface_brightness(index);
	else source_pixel_grid->update_surface_brightness(index);
	if (include_imgfluxes_in_inversion) {
		index = source_npixels;
		for (j=0; j < point_imgs.size(); j++) {
			for (i=0; i < point_imgs[j].size(); i++) {
				point_imgs[j][i].flux = source_pixel_vector[index++];
			}
		}
	}
#endif
}

void QLens::invert_lens_mapping_MUMPS(bool verbal, bool use_copy)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*group_comm, *mpi_group, &sub_comm);
#endif

#ifdef USE_MPI
	MPI_Comm this_comm;
	MPI_Comm_create(*my_comm, *my_group, &this_comm);
#endif

#ifndef USE_MUMPS
	die("QLens requires compilation with MUMPS for Cholesky factorization");
#else

	int default_nthreads=1;

#ifdef USE_OPENMP
	#pragma omp parallel
	{
		#pragma omp master
		default_nthreads = omp_get_num_threads();
	}
	omp_set_num_threads(inversion_nthreads);
#endif

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int i,j;
	double *Fmatptr = (use_copy==true) ? Fmatrix_copy : Fmatrix;

	double *temp = new double[source_n_amps];
	MUMPS_INT Fmatrix_nonzero_elements = Fmatrix_index[source_n_amps]-1;
	if (Fmatrix_nonzero_elements==0) {
		cout << "nsource_pixels=" << source_n_amps << endl;
		die("Fmatrix has zero size");
	}
	MUMPS_INT *irn = new MUMPS_INT[Fmatrix_nonzero_elements];
	MUMPS_INT *jcn = new MUMPS_INT[Fmatrix_nonzero_elements];
	double *Fmatrix_elements = new double[Fmatrix_nonzero_elements];
	for (i=0; i < source_n_amps; i++) {
		Fmatrix_elements[i] = Fmatptr[i];
		irn[i] = i+1;
		jcn[i] = i+1;
		temp[i] = Dvector[i];
	}
	int indx=source_n_amps;
	for (i=0; i < source_n_amps; i++) {
		for (j=Fmatrix_index[i]; j < Fmatrix_index[i+1]; j++) {
			Fmatrix_elements[indx] = Fmatptr[j];
			irn[indx] = i+1;
			jcn[indx] = Fmatrix_index[j]+1;
			indx++;
		}
	}

#ifdef USE_MPI
	if (use_mumps_subcomm) {
		mumps_solver->comm_fortran=(MUMPS_INT) MPI_Comm_c2f(sub_comm);
	} else {
		mumps_solver->comm_fortran=(MUMPS_INT) MPI_Comm_c2f(this_comm);
	}
#endif
	mumps_solver->job = JOB_INIT; // initialize
	mumps_solver->sym = 2; // specifies that matrix is symmetric and positive-definite
	//cout << "ICNTL = " << mumps_solver->icntl[13] << endl;
	dmumps_c(mumps_solver);
	mumps_solver->n = source_n_amps; mumps_solver->nz = Fmatrix_nonzero_elements; mumps_solver->irn=irn; mumps_solver->jcn=jcn;
	mumps_solver->a = Fmatrix_elements; mumps_solver->rhs = temp;
	if (show_mumps_info) {
		mumps_solver->icntl[0] = MUMPS_OUTPUT;
		mumps_solver->icntl[1] = MUMPS_OUTPUT;
		mumps_solver->icntl[2] = MUMPS_OUTPUT;
		mumps_solver->icntl[3] = MUMPS_OUTPUT;
	} else {
		mumps_solver->icntl[0] = MUMPS_SILENT;
		mumps_solver->icntl[1] = MUMPS_SILENT;
		mumps_solver->icntl[2] = MUMPS_SILENT;
		mumps_solver->icntl[3] = MUMPS_SILENT;
	}
	if (regularization_method != None) mumps_solver->icntl[32]=1; // specifies to calculate determinant
	else mumps_solver->icntl[32] = 0;
	if (parallel_mumps) {
		mumps_solver->icntl[27]=2; // parallel analysis phase
		mumps_solver->icntl[28]=2; // parallel analysis phase
	}
	mumps_solver->job = 6; // specifies to factorize and solve linear equation
#ifdef USE_MPI
	MPI_Barrier(sub_comm);
#endif
	dmumps_c(mumps_solver);
#ifdef USE_MPI
	if (use_mumps_subcomm) {
		MPI_Bcast(temp,source_n_amps,MPI_DOUBLE,0,sub_comm);
		MPI_Barrier(sub_comm);
	}
#endif

	if (mumps_solver->info[0] < 0) {
		if (mumps_solver->info[0]==-10) die("Singular matrix, cannot invert");
		else warn("Error occurred during matrix inversion; MUMPS error code %i (source_n_amps=%i)",mumps_solver->info[0],source_n_amps);
	}

	if ((n_image_prior) or (outside_sb_prior)) {
		max_pixel_sb=-1e30;
		int max_sb_i;
		for (int i=0; i < source_n_amps; i++) {
			//if ((data_pixel_noise==0) and (temp[i] < 0)) temp[i] = 0; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			//if (temp[i] < -0.05) temp[i] = -0.05; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			source_pixel_vector[i] = temp[i];
			if (source_pixel_vector[i] > max_pixel_sb) {
				max_pixel_sb = source_pixel_vector[i];
				max_sb_i = i;
			}
		}
		if ((n_image_prior) and (source_fit_mode==Cartesian_Source)) {
			n_images_at_sbmax = source_pixel_n_images[max_sb_i];
			pixel_avg_n_image = 0;
			double sbtot = 0;
			for (int i=0; i < source_n_amps; i++) {
				if (source_pixel_vector[i] >= max_pixel_sb*n_image_prior_sb_frac) {
					pixel_avg_n_image += source_pixel_n_images[i]*source_pixel_vector[i];
					sbtot += source_pixel_vector[i];
				}
			}
			if (sbtot != 0) pixel_avg_n_image /= sbtot;
		}
	} else {
		for (int i=0; i < source_n_amps; i++) {
			if ((data_pixel_noise==0) and (temp[i] < 0)) temp[i] = 0; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			source_pixel_vector[i] = temp[i];
		}
	}

	if (regularization_method != None)
	{
		Fmatrix_log_determinant = log(mumps_solver->rinfog[11]) + mumps_solver->infog[33]*log(2);
		//cout << "Fmatrix log determinant = " << Fmatrix_log_determinant << endl;
		if ((mpi_id==0) and (verbal)) cout << "log determinant = " << Fmatrix_log_determinant << endl;

		mumps_solver->job=JOB_END; dmumps_c(mumps_solver); //Terminate instance

		MUMPS_INT Rmatrix_nonzero_elements = Rmatrix_index[source_n_amps]-1;
		MUMPS_INT *irn_reg = new MUMPS_INT[Rmatrix_nonzero_elements];
		MUMPS_INT *jcn_reg = new MUMPS_INT[Rmatrix_nonzero_elements];
		double *Rmatrix_elements = new double[Rmatrix_nonzero_elements];
		for (i=0; i < source_n_amps; i++) {
			Rmatrix_elements[i] = Rmatrix[i];
			irn_reg[i] = i+1;
			jcn_reg[i] = i+1;
		}
		indx=source_n_amps;
		for (i=0; i < source_n_amps; i++) {
			//cout << "Row " << i << ": diag=" << Rmatrix[i] << endl;
			//for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
				//cout << Rmatrix_index[j] << " ";
			//}
			//cout << endl;
			for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
				//cout << Rmatrix[j] << " ";
				Rmatrix_elements[indx] = Rmatrix[j];
				irn_reg[indx] = i+1;
				jcn_reg[indx] = Rmatrix_index[j]+1;
				indx++;
			}
		}

		mumps_solver->job=JOB_INIT; mumps_solver->sym=2;
		dmumps_c(mumps_solver);
		mumps_solver->n = source_n_amps; mumps_solver->nz = Rmatrix_nonzero_elements; mumps_solver->irn=irn_reg; mumps_solver->jcn=jcn_reg;
		mumps_solver->a = Rmatrix_elements;
		mumps_solver->icntl[0]=MUMPS_SILENT;
		mumps_solver->icntl[1]=MUMPS_SILENT;
		mumps_solver->icntl[2]=MUMPS_SILENT;
		mumps_solver->icntl[3]=MUMPS_SILENT;
		mumps_solver->icntl[32]=1; // calculate determinant
		mumps_solver->icntl[30]=1; // discard factorized matrices
		if (parallel_mumps) {
			mumps_solver->icntl[27]=2; // parallel analysis phase
			mumps_solver->icntl[28]=2; // parallel analysis phase
		}
		mumps_solver->job=4;
		dmumps_c(mumps_solver);
		if (mumps_solver->rinfog[11]==0) Rmatrix_log_determinant = -1e20;
		else Rmatrix_log_determinant = log(mumps_solver->rinfog[11]) + mumps_solver->infog[33]*log(2);
		//cout << "Rmatrix log determinant = " << Rmatrix_log_determinant << " " << mumps_solver->rinfog[11] << " " << mumps_solver->infog[33] << endl;
		if ((mpi_id==0) and (verbal)) cout << "Rmatrix log determinant = " << Rmatrix_log_determinant << " " << mumps_solver->rinfog[11] << " " << mumps_solver->infog[33] << endl;

		delete[] irn_reg;
		delete[] jcn_reg;
		delete[] Rmatrix_elements;
	}
	mumps_solver->job=JOB_END;
	dmumps_c(mumps_solver); //Terminate instance

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for inverting Fmatrix: " << wtime << endl;
	}
#endif

#ifdef USE_OPENMP
	omp_set_num_threads(default_nthreads);
#endif

	delete[] temp;
	delete[] irn;
	delete[] jcn;
	delete[] Fmatrix_elements;
	int index=0;
	if (source_fit_mode==Delaunay_Source) delaunay_srcgrid->update_surface_brightness(index);
	else source_pixel_grid->update_surface_brightness(index);
	if (include_imgfluxes_in_inversion) {
		index = source_npixels;
		for (j=0; j < point_imgs.size(); j++) {
			for (i=0; i < point_imgs[j].size(); i++) {
				point_imgs[j][i].flux = source_pixel_vector[index++];
			}
		}
	}
#endif
#ifdef USE_MPI
	MPI_Comm_free(&sub_comm);
	MPI_Comm_free(&this_comm);
#endif

}

void QLens::Rmatrix_determinant_UMFPACK()
{
#ifndef USE_UMFPACK
	die("QLens requires compilation with UMFPACK (or MUMPS) for determinants of sparse matrices");
#else
	double mantissa, exponent;
	int i,j,status;
   void *Symbolic, *Numeric;
	double Control [UMFPACK_CONTROL];
	double Info [UMFPACK_INFO];
   umfpack_di_defaults (Control);
	Control[UMFPACK_STRATEGY] = UMFPACK_STRATEGY_SYMMETRIC;
	int Rmatrix_nonzero_elements = Rmatrix_index[source_npixels]-1;
	int Rmatrix_n_offdiags = Rmatrix_index[source_npixels]-1-source_npixels;
	int Rmatrix_unsymmetric_nonzero_elements = source_npixels + 2*Rmatrix_n_offdiags;
	if (Rmatrix_nonzero_elements==0) {
		cout << "nsource_pixels=" << source_npixels << endl;
		die("Rmatrix has zero size");
	}

	int k,jl,jm,jp,ju,m,n2,noff,inc,iv;
	double v;

	// Now we construct the transpose of Rmatrix so we can cast it into "unsymmetric" format for UMFPACK (by including offdiagonals on either side of diagonal elements)
	double *Rmatrix_transpose = new double[Rmatrix_nonzero_elements+1];
	int *Rmatrix_transpose_index = new int[Rmatrix_nonzero_elements+1];

	n2=Rmatrix_index[0];
	for (j=0; j < n2-1; j++) Rmatrix_transpose[j] = Rmatrix[j];
	int n_offdiag = Rmatrix_index[n2-1] - Rmatrix_index[0];
	int *offdiag_indx = new int[n_offdiag];
	int *offdiag_indx_transpose = new int[n_offdiag];
	for (i=0; i < n_offdiag; i++) offdiag_indx[i] = Rmatrix_index[n2+i];
	indexx(offdiag_indx,offdiag_indx_transpose,n_offdiag);
	for (j=n2, k=0; j < Rmatrix_index[n2-1]; j++, k++) {
		Rmatrix_transpose_index[j] = offdiag_indx_transpose[k];
	}
	jp=0;
	for (k=Rmatrix_index[0]; k < Rmatrix_index[n2-1]; k++) {
		m = Rmatrix_transpose_index[k] + n2;
		Rmatrix_transpose[k] = Rmatrix[m];
		for (j=jp; j < Rmatrix_index[m]+1; j++)
			Rmatrix_transpose_index[j]=k;
		jp = Rmatrix_index[m] + 1;
		jl=0;
		ju=n2-1;
		while (ju-jl > 1) {
			jm = (ju+jl)/2;
			if (Rmatrix_index[jm] > m) ju=jm; else jl=jm;
		}
		Rmatrix_transpose_index[k]=jl;
	}
	for (j=jp; j < n2; j++) Rmatrix_transpose_index[j] = Rmatrix_index[n2-1];
	for (j=0; j < n2-1; j++) {
		jl = Rmatrix_transpose_index[j+1] - Rmatrix_transpose_index[j];
		noff=Rmatrix_transpose_index[j];
		inc=1;
		do {
			inc *= 3;
			inc++;
		} while (inc <= jl);
		do {
			inc /= 3;
			for (k=noff+inc; k < noff+jl; k++) {
				iv = Rmatrix_transpose_index[k];
				v = Rmatrix_transpose[k];
				m=k;
				while (Rmatrix_transpose_index[m-inc] > iv) {
					Rmatrix_transpose_index[m] = Rmatrix_transpose_index[m-inc];
					Rmatrix_transpose[m] = Rmatrix_transpose[m-inc];
					m -= inc;
					if (m-noff+1 <= inc) break;
				}
				Rmatrix_transpose_index[m] = iv;
				Rmatrix_transpose[m] = v;
			}
		} while (inc > 1);
	}
	delete[] offdiag_indx;
	delete[] offdiag_indx_transpose;

	int *Rmatrix_unsymmetric_cols = new int[source_npixels+1];
	int *Rmatrix_unsymmetric_indices = new int[Rmatrix_unsymmetric_nonzero_elements];
	double *Rmatrix_unsymmetric = new double[Rmatrix_unsymmetric_nonzero_elements];
	int indx=0;
	Rmatrix_unsymmetric_cols[0] = 0;
	for (i=0; i < source_npixels; i++) {
		for (j=Rmatrix_transpose_index[i]; j < Rmatrix_transpose_index[i+1]; j++) {
			Rmatrix_unsymmetric[indx] = Rmatrix_transpose[j];
			Rmatrix_unsymmetric_indices[indx] = Rmatrix_transpose_index[j];
			indx++;
		}
		Rmatrix_unsymmetric_indices[indx] = i;
		Rmatrix_unsymmetric[indx] = Rmatrix[i];
		indx++;
		for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
			Rmatrix_unsymmetric[indx] = Rmatrix[j];
			//cout << "Row " << i << ", column " << Rmatrix_index[j] << ": " << Rmatrix[j] << " " << Rmatrix_unsymmetric[indx] << " (element " << indx << ")" << endl;
			Rmatrix_unsymmetric_indices[indx] = Rmatrix_index[j];
			indx++;
		}
		Rmatrix_unsymmetric_cols[i+1] = indx;
	}

	for (i=0; i < source_npixels; i++) {
		sort(Rmatrix_unsymmetric_cols[i+1]-Rmatrix_unsymmetric_cols[i],Rmatrix_unsymmetric_indices+Rmatrix_unsymmetric_cols[i],Rmatrix_unsymmetric+Rmatrix_unsymmetric_cols[i]);
		//cout << "Row " << i << ": " << endl;
		//cout << Rmatrix_unsymmetric_cols[i] << " ";
		//for (j=Rmatrix_unsymmetric_cols[i]; j < Rmatrix_unsymmetric_cols[i+1]; j++) {
			//cout << Rmatrix_unsymmetric_indices[j] << " ";
		//}
		//cout << endl;
		//for (j=Rmatrix_unsymmetric_cols[i]; j < Rmatrix_unsymmetric_cols[i+1]; j++) {
			//cout << Rmatrix_unsymmetric[j] << " ";
		//}
		//cout << endl;
	}
	//cout << endl;

	if (indx != Rmatrix_unsymmetric_nonzero_elements) die("WTF! Wrong number of nonzero elements");

	status = umfpack_di_symbolic(source_npixels, source_npixels, Rmatrix_unsymmetric_cols, Rmatrix_unsymmetric_indices, Rmatrix_unsymmetric, &Symbolic, Control, Info);
	if (status < 0) {
		umfpack_di_report_info (Control, Info) ;
		umfpack_di_report_status (Control, status) ;
		die("Error inputting matrix");
	}
	status = umfpack_di_numeric(Rmatrix_unsymmetric_cols, Rmatrix_unsymmetric_indices, Rmatrix_unsymmetric, Symbolic, &Numeric, Control, Info);
	if (status < 0) {
		umfpack_di_report_info (Control, Info) ;
		umfpack_di_report_status (Control, status) ;
		die("Error inputting matrix");
	}
	umfpack_di_free_symbolic(&Symbolic);

	status = umfpack_di_get_determinant (&mantissa, &exponent, Numeric, Info) ;
	//cout << "Rmatrix mantissa=" << mantissa << ", exponent=" << exponent << endl;
	if (status < 0) {
		die("Could not calculate determinant");
	}
	Rmatrix_log_determinant = log(mantissa) + exponent*log(10);
	cout << "Rmatrix_logdet=" << Rmatrix_log_determinant << endl;
	delete[] Rmatrix_transpose;
	delete[] Rmatrix_transpose_index;
	delete[] Rmatrix_unsymmetric_cols;
	delete[] Rmatrix_unsymmetric_indices;
	delete[] Rmatrix_unsymmetric;
	umfpack_di_free_numeric(&Numeric);
#endif
}

void QLens::Rmatrix_determinant_MUMPS()
{
#ifndef USE_MUMPS
	die("QLens requires compilation with UMFPACK (or MUMPS) for determinants of sparse matrices");
#else
	int i,j;
	MUMPS_INT Rmatrix_nonzero_elements = Rmatrix_index[source_npixels]-1;
	MUMPS_INT *irn_reg = new MUMPS_INT[Rmatrix_nonzero_elements];
	MUMPS_INT *jcn_reg = new MUMPS_INT[Rmatrix_nonzero_elements];
	double *Rmatrix_elements = new double[Rmatrix_nonzero_elements];
	for (i=0; i < source_npixels; i++) {
		Rmatrix_elements[i] = Rmatrix[i];
		irn_reg[i] = i+1;
		jcn_reg[i] = i+1;
	}
	int indx=source_npixels;
	for (i=0; i < source_npixels; i++) {
		//cout << "Row " << i << ": diag=" << Rmatrix[i] << endl;
		//for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
			//cout << Rmatrix_index[j] << " ";
		//}
		//cout << endl;
		for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
			//cout << Rmatrix[j] << " ";
			Rmatrix_elements[indx] = Rmatrix[j];
			irn_reg[indx] = i+1;
			jcn_reg[indx] = Rmatrix_index[j]+1;
			indx++;
		}
	}

	mumps_solver->job=JOB_INIT; mumps_solver->sym=2;
	dmumps_c(mumps_solver);
	mumps_solver->n = source_npixels; mumps_solver->nz = Rmatrix_nonzero_elements; mumps_solver->irn=irn_reg; mumps_solver->jcn=jcn_reg;
	mumps_solver->a = Rmatrix_elements;
	mumps_solver->icntl[0]=MUMPS_SILENT;
	mumps_solver->icntl[1]=MUMPS_SILENT;
	mumps_solver->icntl[2]=MUMPS_SILENT;
	mumps_solver->icntl[3]=MUMPS_SILENT;
	mumps_solver->icntl[32]=1; // calculate determinant
	mumps_solver->icntl[30]=1; // discard factorized matrices
	if (parallel_mumps) {
		mumps_solver->icntl[27]=2; // parallel analysis phase
		mumps_solver->icntl[28]=2; // parallel analysis phase
	}
	mumps_solver->job=4;
	dmumps_c(mumps_solver);
	if (mumps_solver->rinfog[11]==0) Rmatrix_log_determinant = -1e20;
	else Rmatrix_log_determinant = log(mumps_solver->rinfog[11]) + mumps_solver->infog[33]*log(2);
	//cout << "Rmatrix log determinant = " << Rmatrix_log_determinant << " " << mumps_solver->rinfog[11] << " " << mumps_solver->infog[33] << endl;
	//if (mpi_id==0) cout << "Rmatrix log determinant = " << Rmatrix_log_determinant << " " << mumps_solver->rinfog[11] << " " << mumps_solver->infog[33] << endl;

	delete[] irn_reg;
	delete[] jcn_reg;
	delete[] Rmatrix_elements;
	mumps_solver->job=JOB_END;
	dmumps_c(mumps_solver); //Terminate instance
#endif
}

#define ISWAP(a,b) temp=(a);(a)=(b);(b)=temp;
void QLens::indexx(int* arr, int* indx, int nn)
{
	const int M=7, NSTACK=50;
	int i,indxt,ir,j,k,jstack=-1,l=0;
	double a,temp;
	int *istack = new int[NSTACK];
	ir = nn - 1;
	for (j=0; j < nn; j++) indx[j] = j;
	for (;;) {
		if (ir-l < M) {
			for (j=l+1; j <= ir; j++) {
				indxt=indx[j];
				a=arr[indxt];
				for (i=j-1; i >=l; i--) {
					if (arr[indx[i]] <= a) break;
					indx[i+1]=indx[i];
				}
				indx[i+1]=indxt;
			}
			if (jstack < 0) break;
			ir=istack[jstack--];
			l=istack[jstack--];
		} else {
			k=(l+ir) >> 1;
			ISWAP(indx[k],indx[l+1]);
			if (arr[indx[l]] > arr[indx[ir]]) {
				ISWAP(indx[l],indx[ir]);
			}
			if (arr[indx[l+1]] > arr[indx[ir]]) {
				ISWAP(indx[l+1],indx[ir]);
			}
			if (arr[indx[l]] > arr[indx[l+1]]) {
				ISWAP(indx[l],indx[l+1]);
			}
			i=l+1;
			j=ir;
			indxt=indx[l+1];
			a=arr[indxt];
			for (;;) {
				do i++; while (arr[indx[i]] < a);
				do j--; while (arr[indx[j]] > a);
				if (j < i) break;
				ISWAP(indx[i],indx[j]);
			}
			indx[l+1]=indx[j];
			indx[j]=indxt;
			jstack += 2;
			if (jstack >= NSTACK) die("NSTACK too small in indexx");
			if (ir-i+1 >= j-l) {
				istack[jstack]=ir;
				istack[jstack-1]=i;
				ir=j-1;
			} else {
				istack[jstack]=j-1;
				istack[jstack-1]=l;
				l=i;
			}
		}
	}
	delete[] istack;
}
#undef ISWAP

void QLens::Rmatrix_determinant_MKL()
{
#ifndef USE_MKL
	die("QLens requires compilation with MKL (or UMFPACK or MUMPS) for determinants of sparse matrices");
#else
	// MKL should use Pardiso to get the Cholesky decomposition, but for the moment, I will just convert to dense matrix and do it that way
	if (!dense_Rmatrix) convert_Rmatrix_to_dense();
	int ntot = Rmatrix_packed.size();
	if (ntot != (source_npixels*(source_npixels+1)/2)) die("Rmatrix packed does not have correct number of elements");
	double *Rmatrix_packed_copy = new double[ntot];
	for (int i=0; i < ntot; i++) Rmatrix_packed_copy[i] = Rmatrix_packed[i];
   LAPACKE_dpptrf(LAPACK_ROW_MAJOR,'U',source_npixels,Rmatrix_packed_copy); // Cholesky decomposition
	Cholesky_logdet_packed(Rmatrix_packed_copy,Rmatrix_log_determinant,source_npixels);
	delete[] Rmatrix_packed_copy;
#endif
}

void QLens::convert_Rmatrix_to_dense()
{
	int i,j,indx;
	int ntot = source_npixels*(source_npixels+1)/2;
	Rmatrix_packed.input_zero(ntot);
	indx=0;
	for (i=0; i < source_npixels; i++) {
		Rmatrix_packed[indx] = Rmatrix[i];
		//cout << "Rmat: " << Rmatrix[i] << endl;
		for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
			Rmatrix_packed[indx+Rmatrix_index[j]-i] = Rmatrix[j];
		}
		indx += source_npixels-i;
	}
}

void QLens::clear_lensing_matrices()
{
	if (Dvector != NULL) delete[] Dvector;
	if (Fmatrix != NULL) delete[] Fmatrix;
	if (Fmatrix_index != NULL) delete[] Fmatrix_index;
	if (Rmatrix != NULL) delete[] Rmatrix;
	if (Rmatrix_index != NULL) delete[] Rmatrix_index;
	Dvector = NULL;
	Fmatrix = NULL;
	Fmatrix_index = NULL;
	Rmatrix = NULL;
	Rmatrix_index = NULL;
}

void QLens::calculate_image_pixel_surface_brightness(const bool calculate_foreground)
{
	int img_index_j;
	int i,j,k;

	//cout << "SPARSE SB:" << endl;
	//for (j=0; j < source_n_amps; j++) {
		//cout << source_pixel_vector[j] << " ";
	//}
	//cout << endl;


	for (int img_index=0; img_index < image_npixels; img_index++) {
		image_surface_brightness[img_index] = 0;
		for (img_index_j=image_pixel_location_Lmatrix[img_index]; img_index_j < image_pixel_location_Lmatrix[img_index+1]; img_index_j++) {
			image_surface_brightness[img_index] += Lmatrix[img_index_j]*source_pixel_vector[Lmatrix_index[img_index_j]];
		}
		//if (image_surface_brightness[i] < 0) image_surface_brightness[i] = 0;
	}

	if (calculate_foreground) {
		bool at_least_one_foreground_src = false;

		for (k=0; k < n_sb; k++) {
			if (!sb_list[k]->is_lensed) {
				at_least_one_foreground_src = true;
				break;
			}
		}
		if (at_least_one_foreground_src) {
			calculate_foreground_pixel_surface_brightness();
			//add_foreground_to_image_pixel_vector();
			store_foreground_pixel_surface_brightness(); // this stores it in image_pixel_grid->foreground_surface_brightness[i][j]
		} else {
			for (int img_index=0; img_index < image_npixels_fgmask; img_index++) {
				sbprofile_surface_brightness[img_index] = 0;
			}
		}
	}
}

void QLens::calculate_image_pixel_surface_brightness_dense(const bool calculate_foreground)
{
	int i,j,k;

	//cout << "DENSE SB:" << endl;
	//for (j=0; j < source_n_amps; j++) {
		//cout << source_pixel_vector[j] << " ";
	//}
	//cout << endl;
	double maxsb = -1e30;
	for (int i=0; i < image_npixels; i++) {
		image_surface_brightness[i] = 0;
		for (j=0; j < source_n_amps; j++) {
			image_surface_brightness[i] += Lmatrix_dense[i][j]*source_pixel_vector[j];
		}
		//if (image_surface_brightness[i] < 0) image_surface_brightness[i] = 0;
			if (image_surface_brightness[i] > maxsb) maxsb=image_surface_brightness[i];
	}

	if (calculate_foreground) {
		bool at_least_one_foreground_src = false;
		for (k=0; k < n_sb; k++) {
			if (!sb_list[k]->is_lensed) {
				at_least_one_foreground_src = true;
			}
		}
		if (at_least_one_foreground_src) {
			calculate_foreground_pixel_surface_brightness();
			store_foreground_pixel_surface_brightness(); // this stores it in image_pixel_grid->sbprofile_surface_brightness[i][j]
			//add_foreground_to_image_pixel_vector();
		} else {
			for (int img_index=0; img_index < image_npixels_fgmask; img_index++) sbprofile_surface_brightness[img_index] = 0;
		}
	}
}

void QLens::calculate_foreground_pixel_surface_brightness(const bool allow_lensed_nonshapelet_sources)
{
	bool subgridded;
	int img_index;
	int i,j,k;
	bool at_least_one_foreground_src = false;
	for (k=0; k < n_sb; k++) {
		if (!sb_list[k]->is_lensed) {
			at_least_one_foreground_src = true;
		} 
	}

	/*	
	for (int img_index=0; img_index < image_npixels_fgmask; img_index++) {
		//cout << img_index << endl;
		sbprofile_surface_brightness[img_index] = 0;

		i = active_image_pixel_i_fgmask[img_index];
		j = active_image_pixel_j_fgmask[img_index];

		cout << i << " " << j << " " << image_pixel_grid->x_N << " " << image_pixel_grid->y_N << endl;
		cout << image_pixel_grid->center_pts[i][j][0] << " " << image_pixel_grid->center_pts[i][j][1] << endl;
	}
	die();
	*/


	//for (i=0; i < image_pixel_grid->x_N; i++) {
		//for (j=0; j < image_pixel_grid->y_N; j++) {
			//cout << i << " " << j << " " << image_pixel_grid->x_N << " " << image_pixel_grid->y_N << endl;
			//cout << image_pixel_grid->center_pts[i][j][0] << " " << image_pixel_grid->center_pts[i][j][1] << endl;
		//}
	//}
	//cout << "DONE" << endl;
	//die();

	// here, we are adding together SB of foreground sources, but also lensed non-shapelet sources if we're in shapelet mode.
	// If none of those conditions are true, then we skip everything.
	if (!at_least_one_foreground_src) {
		for (img_index=0; img_index < image_npixels_fgmask; img_index++) sbprofile_surface_brightness[img_index] = 0;
		return;
	} else {
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif

			int ii, jj, nsplit;
			double u0, w0, sb;
			//double U0, W0, U1, W1;
			lensvector center_pt, center_srcpt;
			lensvector corner1, corner2, corner3, corner4;
			lensvector corner1_src, corner2_src, corner3_src, corner4_src;
			double subpixel_xlength, subpixel_ylength;
			int subpixel_index;
			#pragma omp for private(img_index,i,j,ii,jj,nsplit,u0,w0,sb,subpixel_xlength,subpixel_ylength,center_pt,center_srcpt,corner1,corner2,corner3,corner4,corner1_src,corner2_src,corner3_src,corner4_src,subpixel_index) schedule(dynamic)
			for (img_index=0; img_index < image_npixels_fgmask; img_index++) {
				sbprofile_surface_brightness[img_index] = 0;

				i = active_image_pixel_i_fgmask[img_index];
				j = active_image_pixel_j_fgmask[img_index];

				sb = 0;

				if (split_imgpixels) nsplit = image_pixel_grid->nsplits[i][j];
				else nsplit = 1;
				// Now check to see if center of foreground galaxy is in or next to the pixel; if so, make sure it has at least four splittings so its
				// surface brightness is well-reproduced
				if ((nsplit < 4) and (i > 0) and (i < image_pixel_grid->x_N-1) and (j > 0) and (j < image_pixel_grid->y_N)) {
					for (k=0; k < n_sb; k++) {
						if (!sb_list[k]->is_lensed) {
							double xc, yc;
							sb_list[k]->get_center_coords(xc,yc);
							if ((xc > image_pixel_grid->corner_pts[i-1][j][0]) and (xc < image_pixel_grid->corner_pts[i+2][j][0]) and (yc > image_pixel_grid->corner_pts[i][j-1][1]) and (yc < image_pixel_grid->corner_pts[i][j+2][1])) nsplit = 4;
						} 
					}
				}

				subpixel_xlength = image_pixel_grid->pixel_xlength/nsplit;
				subpixel_ylength = image_pixel_grid->pixel_ylength/nsplit;
				subpixel_index = 0;
				for (ii=0; ii < nsplit; ii++) {
					u0 = ((double) (1+2*ii))/(2*nsplit);
					center_pt[0] = u0*image_pixel_grid->corner_pts[i][j][0] + (1-u0)*image_pixel_grid->corner_pts[i+1][j][0];
					for (jj=0; jj < nsplit; jj++) {
						w0 = ((double) (1+2*jj))/(2*nsplit);
						center_pt[1] = w0*image_pixel_grid->corner_pts[i][j][1] + (1-w0)*image_pixel_grid->corner_pts[i][j+1][1];
						//center_pt = image_pixel_grid->subpixel_center_pts[i][j][subpixel_index]; 
						//cout << "CHECK: " << image_pixel_grid->subpixel_center_pts[i][j][subpixel_index][0] << " " << center_pt[0] << " and " << image_pixel_grid->subpixel_center_pts[i][j][subpixel_index][1] << " " << center_pt[1] << endl;
						for (int k=0; k < n_sb; k++) {
							if ((!sb_list[k]->is_lensed) and ((source_fit_mode != Shapelet_Source) or (sb_list[k]->sbtype != SHAPELET))) {
								if (!sb_list[k]->zoom_subgridding) sb += sb_list[k]->surface_brightness(center_pt[0],center_pt[1]);
								else {
									corner1[0] = center_pt[0] - subpixel_xlength/2;
									corner1[1] = center_pt[1] - subpixel_ylength/2;
									corner2[0] = center_pt[0] + subpixel_xlength/2;
									corner2[1] = center_pt[1] - subpixel_ylength/2;
									corner3[0] = center_pt[0] - subpixel_xlength/2;
									corner3[1] = center_pt[1] + subpixel_ylength/2;
									corner4[0] = center_pt[0] + subpixel_xlength/2;
									corner4[1] = center_pt[1] + subpixel_ylength/2;
									sb += sb_list[k]->surface_brightness_zoom(center_pt,corner1,corner2,corner3,corner4);
								}
							}
							else if ((allow_lensed_nonshapelet_sources) and (sb_list[k]->is_lensed) and (sb_list[k]->sbtype != SHAPELET) and (image_pixel_data->extended_mask[i][j])) { // if source mode is shapelet and sbprofile is shapelet, will include in inversion
								//center_srcpt = image_pixel_grid->subpixel_center_sourcepts[i][j][subpixel_index];
								//center_srcpt = image_pixel_grid->subpixel_center_sourcepts[i][j][subpixel_index];
								//find_sourcept(center_pt,center_srcpt,thread,reference_zfactors,default_zsrc_beta_factors);
								//sb += sb_list[k]->surface_brightness(center_srcpt[0],center_srcpt[1]);
								if (split_imgpixels) sb += sb_list[k]->surface_brightness(image_pixel_grid->subpixel_center_sourcepts[i][j][subpixel_index][0],image_pixel_grid->subpixel_center_sourcepts[i][j][subpixel_index][1]);
								else sb += sb_list[k]->surface_brightness(image_pixel_grid->center_sourcepts[i][j][0],image_pixel_grid->center_sourcepts[i][j][1]);
								//if (!sb_list[k]->zoom_subgridding) sb += sb_list[k]->surface_brightness(center_srcpt[0],center_srcpt[1]);
								//else {
									//corner1[0] = center_srcpt[0] - subpixel_xlength/2;
									//corner1[1] = center_srcpt[1] - subpixel_ylength/2;
									//corner2[0] = center_srcpt[0] + subpixel_xlength/2;
									//corner2[1] = center_srcpt[1] - subpixel_ylength/2;
									//corner3[0] = center_srcpt[0] - subpixel_xlength/2;
									//corner3[1] = center_srcpt[1] + subpixel_ylength/2;
									//corner4[0] = center_srcpt[0] + subpixel_xlength/2;
									//corner4[1] = center_srcpt[1] + subpixel_ylength/2;
									//sb += sb_list[k]->surface_brightness_zoom(center_srcpt,corner1,corner2,corner3,corner4);
								//}
							}
						}
						subpixel_index++;
					}
				}
				sbprofile_surface_brightness[img_index] += sb / (nsplit*nsplit);
			}
		}
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating foreground SB: " << wtime << endl;
	}
#endif

	}
	PSF_convolution_pixel_vector(sbprofile_surface_brightness,true,false);
}

void QLens::add_foreground_to_image_pixel_vector()
{
	for (int img_index=0; img_index < image_npixels; img_index++) {
		image_surface_brightness[img_index] += sbprofile_surface_brightness[img_index];
	}
}

void QLens::store_image_pixel_surface_brightness()
{
	int i,j;
	for (i=0; i < image_pixel_grid->x_N; i++)
		for (j=0; j < image_pixel_grid->y_N; j++)
			image_pixel_grid->surface_brightness[i][j] = 0;

	for (int img_index=0; img_index < image_npixels; img_index++) {
		i = active_image_pixel_i[img_index];
		j = active_image_pixel_j[img_index];
		image_pixel_grid->surface_brightness[i][j] = image_surface_brightness[img_index];
	}
}

void QLens::store_foreground_pixel_surface_brightness() // note, foreground_surface_brightness could also include source objects that aren't shapelets (if in shapelet mode)
{
	int i,j;
	for (int img_index=0; img_index < image_npixels_fgmask; img_index++) {
		i = active_image_pixel_i_fgmask[img_index];
		j = active_image_pixel_j_fgmask[img_index];
		image_pixel_grid->foreground_surface_brightness[i][j] = sbprofile_surface_brightness[img_index];
	}
}

void QLens::vectorize_image_pixel_surface_brightness(bool use_mask)
{
	int i,j,k=0;
	if (active_image_pixel_i == NULL) {
		//	delete[] active_image_pixel_i;
		//if (active_image_pixel_j == NULL) delete[] active_image_pixel_j;
		if (use_mask) {
			int n=0;
			for (j=0; j < image_pixel_grid->y_N; j++) {
				for (i=0; i < image_pixel_grid->x_N; i++) {
					if ((image_pixel_grid->fit_to_data != NULL) and (image_pixel_grid->fit_to_data[i][j])) n++;
				}
			}
			image_npixels = n;
		} else {
			image_npixels = image_pixel_grid->x_N*image_pixel_grid->y_N;
		}
		active_image_pixel_i = new int[image_npixels];
		active_image_pixel_j = new int[image_npixels];
		for (j=0; j < image_pixel_grid->y_N; j++) {
			for (i=0; i < image_pixel_grid->x_N; i++) {
				if ((!use_mask) or ((image_pixel_grid->fit_to_data != NULL) and (image_pixel_grid->fit_to_data[i][j]))) {
					active_image_pixel_i[k] = i;
					active_image_pixel_j[k] = j;
					image_pixel_grid->pixel_index[i][j] = k++;
				}
			}
		}
	}
	if (image_surface_brightness != NULL) delete[] image_surface_brightness;
	image_surface_brightness = new double[image_npixels];

	for (k=0; k < image_npixels; k++) {
		i = active_image_pixel_i[k];
		j = active_image_pixel_j[k];
		image_surface_brightness[k] = image_pixel_grid->surface_brightness[i][j];
	}
}

void QLens::plot_image_pixel_surface_brightness(string outfile_root)
{
	string sb_filename = outfile_root + ".dat";
	string x_filename = outfile_root + ".x";
	string y_filename = outfile_root + ".y";

	ofstream xfile; open_output_file(xfile,x_filename);
	for (int i=0; i <= image_pixel_grid->x_N; i++) {
		xfile << image_pixel_grid->corner_pts[i][0][0] << endl;
	}

	ofstream yfile; open_output_file(yfile,y_filename);
	for (int i=0; i <= image_pixel_grid->y_N; i++) {
		yfile << image_pixel_grid->corner_pts[0][i][1] << endl;
	}

	ofstream surface_brightness_file; open_output_file(surface_brightness_file,sb_filename);
	int index=0;
	for (int j=0; j < image_pixel_grid->y_N; j++) {
		for (int i=0; i < image_pixel_grid->x_N; i++) {
			if ((image_pixel_grid->maps_to_source_pixel[i][j]) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[i][j])))
				surface_brightness_file << image_surface_brightness[index++] << " ";
			else surface_brightness_file << "0 ";
		}
		surface_brightness_file << endl;
	}
}

