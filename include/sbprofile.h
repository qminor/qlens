#ifndef SBPROFILE_H
#define SBPROFILE_H

#include "mathexpr.h"
#include "spline.h"
#include "lensvec.h"
#include "profile.h"
#include <cmath>
#include <iostream>
#include <vector>
using namespace std;

enum SB_ProfileName { SB_SPLINE, GAUSSIAN, SERSIC, CORED_SERSIC, SHAPELET, TOPHAT, SB_MULTIPOLE };

class SB_Profile
{
	friend class QLens;
	private:
	Spline sb_spline;
	double sb_splint(double);
	double qx_parameter, f_parameter;

	protected:
	SB_ProfileName sbtype;
	double q, theta, x_center, y_center; // four base parameters, which can be added to in derived surface brightness models
	double epsilon, epsilon2; // used for defining ellipticity, or else components of ellipticity (epsilon, epsilon2)
	double c0; // "boxiness" parameter
	double rt; // truncation radius parameter
	double costheta, sintheta;

	double **param; // this is an array of pointers, each of which points to the corresponding indexed parameter for each model

	int n_params, n_vary_params;
	int angle_paramnum; // used to keep track of angle parameter so it can be easily converted to degrees and displayed
	bool include_boxiness_parameter;
	bool include_truncation_radius;
	bool include_contour_bumps; // localized isophote perturbation
	boolvector vary_params;
	string model_name;
	vector<string> paramnames;
	vector<string> latex_paramnames, latex_param_subscripts;
	boolvector set_auto_penalty_limits;
	dvector penalty_upper_limits, penalty_lower_limits;
	dvector stepsizes;
	bool include_limits;
	dvector lower_limits, upper_limits;
	dvector lower_limits_initial, upper_limits_initial;

	int n_fourier_modes; // Number of Fourier mode perturbations to elliptical isophotes (zero by default)
	ivector fourier_mode_mvals, fourier_mode_paramnum;
	dvector fourier_mode_cosamp, fourier_mode_sinamp;

	int n_contour_bumps; // Number of contour "bump" perturbations to elliptical isophotes (zero by default)
	ivector bump_paramnum;
	dvector bump_drvals, bump_xcvals, bump_ycvals, bump_sigvals, bump_e1vals, bump_e2vals;
	dvector bump_qvals, bump_phivals, bump_xcprime, bump_ycprime; // meta-parameters

	void set_nparams(const int &n_params_in);
	void copy_base_source_data(const SB_Profile* sb_in);

	void set_geometric_param_pointers(int qi);
	void set_geometric_paramnames(int qi);
	void set_ellipticity_parameter(const double &q_in);

	void set_geometric_parameters(const double &q_in, const double &theta_degrees, const double &xc_in, const double &yc_in);
	void set_geometric_parameters_radians(const double &q_in, const double &theta_in, const double &xc_in, const double &yc_in);
	void set_angle_from_components(const double &comp_x, const double &comp_y);
	void calculate_ellipticity_components();
	void update_meta_parameters_and_pointers();
	void update_angle_meta_params();
	void update_ellipticity_meta_parameters();
	virtual void update_meta_parameters()
	{
		update_ellipticity_meta_parameters();
	}

	void set_angle(const double &theta_degrees);
	void set_angle_radians(const double &theta_in);
	void rotate(double&, double&);
	void rotate_back(double&, double&);
	static bool orient_major_axis_north;
	static bool use_sb_ellipticity_components; // if set to true, uses e_1 and e_2 as fit parameters instead of gamma and theta
	static bool use_fmode_scaled_amplitudes; // if set to true, uses a_m = m*A_m and b_m = m*B_m as parameters instead of true amplitudes
	static double zoom_split_factor; 
	static double zoom_scale; 

	public:
	int sb_number;
	bool is_lensed; // Can be a lensed source, or a galaxy in the lens plane
	bool zoom_subgridding; // Useful if pixels are large compared to profile--subgrids to prevent undersampling
	bool center_anchored;
	LensProfile* center_anchor_lens;
	int *indxptr; // points to important integer values for subclasses that uses them (e.g. shapelets)

	SB_Profile() : qx_parameter(1), param(0) {}
	SB_Profile(const char *splinefile, const double &q_in, const double &theta_degrees, const double &xc_in, const double &yc_in, const double &qx_in, const double &f_in);
	SB_Profile(const SB_Profile* sb_in);
	~SB_Profile() {
		if (param != NULL) delete[] param;
	}

	void anchor_center_to_lens(LensProfile** center_anchor_list, const int &center_anchor_lens_number);
	void delete_center_anchor();
	virtual void assign_param_pointers();
	virtual void assign_paramnames();
	bool vary_parameters(const boolvector& vary_params_in);
	void add_fourier_mode(const int m_in, const double amp_in, const double phi_in, const bool vary1, const bool vary2);
	void add_boxiness_parameter(const double c0_in, const bool vary_c0);
	void add_truncation_radius(const double rt_in, const bool vary_rt);
	void add_contour_bump(const double amp, const double x, const double y, const double sig, const double e1, const double e2);
	void set_lensed(const bool isl) {
		is_lensed = isl;
	}
	void set_zoom_subgridding(const bool zoom) {
		zoom_subgridding = zoom;
	}

	void remove_fourier_modes();

	void set_limits(const dvector& lower, const dvector& upper);
	void set_limits(const dvector& lower, const dvector& upper, const dvector& lower_init, const dvector& upper_init);
	bool get_limits(dvector& lower, dvector& upper, dvector& lower0, dvector& upper0, int &index);
	void shift_angle_90();
	void shift_angle_minus_90();
	void reset_angle_modulo_2pi();
	virtual void set_auto_stepsizes();  // This *must* be redefined in all derived classes
	virtual void set_auto_ranges(); // This *must* be redefined in all derived classes

	void set_auto_eparam_stepsizes(int eparam1_i, int eparam2_i);
	void get_auto_stepsizes(dvector& stepsizes, int &index);
	void set_geometric_param_auto_ranges(int param_i);
	void get_auto_ranges(boolvector& use_penalty_limits, dvector& lower, dvector& upper, int &index);

	virtual void get_fit_parameters(dvector& fitparams, int &index);
	void get_fit_parameter_names(vector<string>& paramnames_vary, vector<string> *latex_paramnames_vary = NULL, vector<string> *latex_subscripts_vary = NULL);
	virtual void get_parameters(double* params);
	bool get_specific_parameter(const string name_in, double& value);
	bool update_specific_parameter(const string name_in, const double& value);
	virtual void update_parameters(const double* params);
	virtual void update_fit_parameters(const double* fitparams, int &index, bool& status);
	void update_anchor_center();
	void print_parameters();
	void print_vary_parameters();
	void output_field_in_sci_notation(double* num, ofstream& scriptout, const bool space);
	virtual void print_source_command(ofstream& scriptout, const bool use_limits);
	virtual bool get_special_command_arg(string &arg);

	// the following items MUST be redefined in all derived classes
	virtual double sb_rsq(const double rsq); // we use the r^2 version in the integrations rather than r because it is most directly used in cored models
	virtual void window_params(double& xmin, double& xmax, double& ymin, double& ymax);
	virtual double window_rmax();
	virtual double length_scale(); // retrieves characteristic length scale of object (used for zoom subgridding)

	// these functions can be redefined in the derived classes, but don't have to be
	virtual double surface_brightness_r(const double r);
	virtual double surface_brightness(double x, double y);
	//virtual double calculate_Lmatrix_element(const double x, const double y, const int amp_index); // used by Shapelet subclass
	virtual void calculate_Lmatrix_elements(double x, double y, double*& Lmatrix_elements, const double weight); // used by Shapelet subclass
	virtual void calculate_gradient_Rmatrix_elements(double*& Rmatrix_elements, double &logdet);
	virtual void update_amplitudes(double*& ampvec); // used by Shapelet subclass
	//virtual void get_amplitudes(double *ampvec); // used by Shapelet subclass
	virtual void update_indxptr(const int newval);
	virtual double surface_brightness_zeroth_order(double x, double y);
	//virtual double surface_brightness_zoom(const double x, const double y, const double pixel_xlength, const double pixel_ylength);
	double surface_brightness_zoom(lensvector &centerpt, lensvector &pt1, lensvector &pt2, lensvector &pt3, lensvector &pt4);

	SB_ProfileName get_sbtype() { return sbtype; }
	void get_center_coords(double &xc, double &yc) { xc=x_center; yc=y_center; }
	int get_n_params() { return n_params; }
	int get_n_vary_params() { return n_vary_params; }
	int get_center_anchor_number() { return center_anchor_lens->lens_number; }
	void set_include_limits(bool inc) { include_limits = inc; }
};

class Gaussian : public SB_Profile
{
	private:
	double sbtot, max_sb, sig_x; // sig_x is the dispersion along the major axis

	double sb_rsq(const double);

	public:
	Gaussian() : SB_Profile() {}
	Gaussian(const double &max_sb_in, const double &sig_x_in, const double &q_in, const double &theta_degrees, const double &xc_in, const double &yc_in);
	Gaussian(const Gaussian* sb_in);
	~Gaussian() {}

	//double surface_brightness_zoom(const double x, const double y, const double pixel_xlength, const double pixel_ylength);
	//double surface_brightness_zoom(lensvector &centerpt, lensvector &pt1, lensvector &pt2, lensvector &pt3, lensvector &pt4);

	void update_meta_parameters();
	void assign_paramnames();
	void assign_param_pointers();
	void set_auto_stepsizes();
	void set_auto_ranges();

	double window_rmax();
	double length_scale();
};

class Sersic : public SB_Profile
{
	private:
	double s0, k, n;
	double Reff; // effective radius

	double sb_rsq(const double);

	public:
	Sersic() : SB_Profile() {}
	Sersic(const double &s0_in, const double &Reff_in, const double &n_in, const double &q_in, const double &theta_degrees, const double &xc_in, const double &yc_in);
	Sersic(const Sersic* sb_in);
	~Sersic() {}

	void update_meta_parameters();
	void assign_paramnames();
	void assign_param_pointers();
	void set_auto_stepsizes();
	void set_auto_ranges();

	void print_parameters();
	double window_rmax();
	double length_scale();
};

class Cored_Sersic : public SB_Profile
{
	private:
	double s0, k, n, rc;
	double Reff; // effective radius

	double sb_rsq(const double);

	public:
	Cored_Sersic() : SB_Profile() {}
	Cored_Sersic(const double &s0_in, const double &Reff_in, const double &n_in, const double &rc_in, const double &q_in, const double &theta_degrees, const double &xc_in, const double &yc_in);
	Cored_Sersic(const Cored_Sersic* sb_in);
	~Cored_Sersic() {}

	void update_meta_parameters();
	void assign_paramnames();
	void assign_param_pointers();
	void set_auto_stepsizes();
	void set_auto_ranges();

	void print_parameters();
	double window_rmax();
	double length_scale();
};

class Shapelet : public SB_Profile
{
	private:
	double sig; // sig is the average dispersion of the (0,0) shapelet which is Gaussian
	double **amps; // shapelet amplitudes
	int n_shapelets;
	bool truncate_at_3sigma; // this truncates the shapelets at r = 2*sigma to eliminate edge effects
	bool nonlinear_amp00;

	public:
	Shapelet() : SB_Profile() { amps = NULL; }
	Shapelet(const double &amp00, const double &sig_in, const double &q_in, const double &theta_degrees, const double &xc_in, const double &yc_in, const int nn, const bool nonlinear_amp00_in, const bool truncate_2sig);
	Shapelet(const Shapelet* sb_in);
	~Shapelet() {
		if (amps != NULL) {
			for (int i=0; i < n_shapelets; i++) delete[] amps[i];
			delete[] amps;
		}
	}

	double surface_brightness(double x, double y);
	double hermite_polynomial(const double x, const int n);

	//double surface_brightness_zoom(const double x, const double y, const double pixel_xlength, const double pixel_ylength);
	//double surface_brightness_zoom(lensvector &centerpt, lensvector &pt1, lensvector &pt2, lensvector &pt3, lensvector &pt4);

	void update_meta_parameters();
	void assign_paramnames();
	void assign_param_pointers();
	void set_auto_stepsizes();
	void set_auto_ranges();
	//double calculate_Lmatrix_element(double x, double y, const int amp_index);
	void calculate_Lmatrix_elements(double x, double y, double*& Lmatrix_elements, const double weight);
	void calculate_gradient_Rmatrix_elements(double*& Rmatrix_elements, double &logdet);
	double surface_brightness_zeroth_order(double x, double y);
	void update_amplitudes(double*& ampvec);
	//void get_amplitudes(double *ampvec);
	void update_indxptr(const int newval);
	bool get_special_command_arg(string &arg);

	double window_rmax();
	double length_scale();
};

class SB_Multipole : public SB_Profile
{
	private:
	int m;
	double A_n, r0, theta_eff;
	bool sine_term; // specifies whether it is a sine or cosine multipole term

	public:
	SB_Multipole() : SB_Profile() {}
	SB_Multipole(const double &A_m_in, const double n_in, const int m_in, const double &theta_degrees, const double &xc_in, const double &yc_in, const bool sine);
	SB_Multipole(const SB_Multipole* sb_in);
	~SB_Multipole() {}

	double surface_brightness(double x, double y);

	void assign_paramnames();
	void assign_param_pointers();
	void set_auto_stepsizes();
	void set_auto_ranges();

	void print_parameters();
	double window_rmax();
	double length_scale();
};

class TopHat : public SB_Profile
{
	private:
	double sb, rad; // sig_x is the dispersion along the major axis

	double sb_rsq(const double);

	public:
	TopHat() : SB_Profile() {}
	TopHat(const double &max_sb_in, const double &sig_x_in, const double &q_in, const double &theta_degrees, const double &xc_in, const double &yc_in);
	TopHat(const TopHat* sb_in);
	~TopHat() {}

	void assign_paramnames();
	void assign_param_pointers();
	void set_auto_stepsizes();
	void set_auto_ranges();

	void print_parameters();
	double window_rmax();
	double length_scale();
};


#endif // SBPROFILE_H
