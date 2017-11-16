# QLens (beta version)
QLens is a software package for modeling and simulating strong gravitational lens systems. Both point image modeling (with option to include fluxes and time delays) and pixel image modeling (using pixellated source reconstruction) are supported. QLens includes 13 different analytic lens models to choose from for model fitting, with an option to load a numerically generated kappa profile using interpolation over a table of kappa values. Chi-square optimization can be performed with the downhill simplex method (plus optional simulated annealing) or Powell's method; for Bayesian parameter estimation, nested sampling or an adaptive Metropolis-Hastings MCMC algorithm (T-Walk) can be used to infer the Bayesian evidence and posteriors. An additional tool, mkdist, generates 1d and 2d posterior plots in each parameter, or if an optimization was done, approximate posteriors can be plotted using the Fisher matrix. The QLens package includes an introductory tutorial [qlens\_tutorial.pdf](qlens_tutorial.pdf) that is meant to be readable for undergraduates and beginning graduate students with little or no experience with gravitational lensing concepts.

Required packages for basic, out-of-the-box configuration:
* GNU Readline &mdash; for command-line interface
* gnuplot &mdash; for generating and viewing plots from within QLens

Optional packages:
* OpenMP &mdash; used for multithreading likelihood evaluations, useful esp. for lens models that require numerical integration for lensing calculations or if source pixel reconstruction is being used.
* MPI &mdash; for running multiple MCMC chains simultaneously using twalk, or increasing acceptance ratio during nested sampling; a subset of MPI processes can also be used to parallelize the likelihood if source pixel reconstruction is used, or if multiple source points are being fit to
* CFITSIO library &mdash; for reading and writing FITS files
* MUMPS package &mdash; for sparse matrix inversion in pixel image modeling (often painful to install however)
* UMFPACK &mdash; alternative to MUMPS; much easier to install but not quite as fast or parallelized

# change log (Nov. 15, 2017)

Upgrades since Nov. 12:

1. There are two new variables that are relevant to using the image plane chi-square: 'chisq\_mag\_threshold' and 'chisq\_imgsep\_threshold'. The latter tells the chi-square to ignore duplicate images with separations smaller than the given threshold, while the former ignores images fainter than the given magnification threshold. Both are zero by default, but probably chisq\_imgsep\_threshold = 1e-3 is a reasonable value in most cases&mdash;I find that the duplicate images always seem to have separations smaller than this. Usually increasing the number of splittings around critical curves (using 'cc\_splitlevels) will get rid of duplicate "phantom" images, but it slows down the image searching. So setting the above threshold may be a useful alternative.

2. The image plane chi-square function (for point image modeling) can now be parallelized with MPI if multiple source points are being fit to. This can significantly reduce the time for each chi-square evaluation if you lens model requires numerical integration to calculate deflections (which is true for the NFW or Sersic models, e.g.). As an example, if you are modeling four source points, you can split the work among up to four MPI processes. To do this, e.g. with four processes, run qlens as follows:

		mpirun -n 4 qlens script.in -g1

	The '-g1' argument tells it to use all four processes for each chi-square evaluation. If you have enough processors for this, it will run 3 or 4 times faster in this example; but even with just two processors, you could run with the '-n 2' argument and it will split up the four source points among the two processes (so two each), nearly doubling the speed of each chi-square evaluation. This gives you a better speedup in comparison to running with multiple OpenMP threads (which speeds up the image searching), but with enough processors, you could combine both approaches to make it even faster. In the above example, if you have eight processors you can use two OpenMP threads (with 'export OMP_NUM_THREADS=2' before you run qlens) and you will get an additional speedup.

	Make sure you are not using too many processes for the machine you're using. If you've compiled QLens with OpenMP, you can test out the time required for each chi-square evaluation by running QLens with the '-w' argument (regardless of whether you're using more than one OpenMP thread or not) and using the 'fit chisq' command; it will spit out the time elapsed. This is highly recommended as it will allow you to experiment with different number of processes/threads until you find the fastest combination, before you run your fit(s).

	If you are using TWalk or nested sampling, you have the option of multiple MPI 'groups', where each group does simultaneous chi-square evaluations (which for T-Walk means you can move multiple chains forward at the same time), while the processes within each group parallelize the chi-square function. For example, if you want two processes per chi-square evaluation, and four MPI groups to move the chains forward, you would then have 8 processes total (again, assuming you have enough processors to do this), so you would run it as

		mpirun -n 8 qlens script.in -g4

	By default, if you don't specify the number of groups with the '-g' argument, QLens will assume the same number of groups as processes (which would be eight in the above example); in other words, it assumes only one process per group unless you tell it otherwise. On a computing cluster you have a lot more processors, hence more freedom to do a combination of all these approaches&mdash;parallelizing the chi-square evaluations, running parallel chains, and multithreading the image searching with OpenMP.

Previous upgrades from Nov. 13 version:

1. General lens parameter anchoring

	General lens parameter anchoring has been implemented, so that you can now anchor a lens parameter to any other lens parameter. To demonstrate this, suppose our first lens is entered as follows:

		fit lens alpha 5 1 0 0.8 30 0 0  
		1 1 0 1 1 1 1

	so that this now becomes listed as lens "0".

	a) Anchor type 1: Now suppose I add another model, e.g. a kappa multipole, where I want the angle to always be equal to that of lens 0. Then I enter this as

		fit lens kmpole 0.1 2 anchor=0,4 0 0  
		1 0 0 1 1

	The "anchor=0,4" means we are anchoring this parameter (the angle) to lens 0, parameter 4 which is the angle of the first lens (remember the first parameter is indexed as zero!). The vary flag must be turned off for the parameter being anchored, or else qlens will complain.

	NOTE: Keep in mind that as long as you use the correct format, qlens will not complain no matter how absurd the choice of anchoring is; so make sure you have indexed it correctly! To test it out, you can use "lens update ..." to update the lens you are anchoring to, and make sure that the anchored parameter changes accordingly.

	b) Anchor type 2: Suppose I want to add a model where I want a parameter to keep the same *ratio* with a parameter in another lens that I started with. You can do this using the following format:

		fit lens alpha 2.5/anchor=0,0 1 0 0.8 30 0 0  
		1 0 0 1 1 1 1

	The "2.5/anchor=0,0" enters the initial value in as 2.5, and since this is half of the parameter we are anchoring to (b=5 for lens 0), they will always keep this ratio. It is even possible to anchor a parameter to another parameter in the *same* lens model, if you use the lens number that will be assigned to the lens you are creating. Again, the vary flag *must* be off for the parameter being anchored.

	We can still anchor the lens's center coordinates to another lens the old way, but in order to distinguish from the above anchoring, now the command is "anchor\_center=...". So in the previous example, if we wanted to also anchor the center of the lens to lens 0, we do

		fit lens alpha 2.5/anchor=0,0 1 0 0.8 30 anchor_center=0  
		1 0 0 1 1 0 0

	The vary flags for the center coordinates must be entered as zeroes, or they can be left off altogether.

2. Fit parameter limits are now assigned default values for certain parameters. For example, in any lens, when you type "fit plimits" you will see that by default, the 'q' parameters have limits from 0 to 1, and so on. This used to be done "under the hood" but now is made explicit using plimits. The plimits are also used to define ranges when plotting approximate posteriors using the Fisher matrix (with 'mkdist ... -fP').

3. You no longer have to load an input script by prefacing with '-f:' before writing the file name. Now you can simply type "qlens script.in" (or whatever you call your script).

4. Bug fix: the vary flags for external shear (when added with "shear=# #" at the end of lens models) were not being set properly for NFW and several other lenses. This has been fixed.
