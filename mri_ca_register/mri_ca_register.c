

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "mri.h"
#include "matrix.h"
#include "proto.h"
#include "macros.h"
#include "error.h"
#include "timer.h"
#include "diag.h"
#include "mrimorph.h"
#include "utils.h"
#include "gca.h"
#include "cma.h"
#include "mrinorm.h"
#include "gcamorph.h"
#include "transform.h"
#include "mrisegment.h"

static int remove_bright =0 ;
static int map_to_flash = 0 ;
static double TRs[MAX_GCA_INPUTS] ;
static double fas[MAX_GCA_INPUTS] ;
static double TEs[MAX_GCA_INPUTS] ;

char         *Progname ;
static GCA_MORPH_PARMS  parms ;

static int avgs = 0 ;  /* for smoothing conditional densities */
static char *mask_fname = NULL ;
static char *norm_fname = NULL ;
static int renormalize = 0 ;

static float regularize = 0 ;
static char *example_T1 = NULL ;
static char *example_segmentation = NULL ;
static int register_wm_flag = 0 ;

static double TR = -1 ;
static double alpha = -1 ;
static double TE = -1 ;
static char *tl_fname = NULL ;

static char *sample_fname = NULL ;
static char *transformed_sample_fname = NULL ;
static char *normalized_transformed_sample_fname = NULL ;
static char *ctl_point_fname = NULL ;
static int novar = 1 ;

static int use_contrast = 0 ;
static float min_prior = MIN_PRIOR ;
static int reset = 0 ;

static FILE *diag_fp = NULL ;

static int translation_only = 0 ;
static int get_option(int argc, char *argv[]) ;
static int write_vector_field(MRI *mri, GCA_MORPH *gcam, char *vf_fname) ;
static int remove_bright_stuff(MRI *mri, GCA *gca, TRANSFORM *transform) ;

static char *renormalization_fname = NULL ;
static char *tissue_parms_fname = NULL ;
static int center = 1 ;
static int nreductions = 1 ;
static char *xform_name = NULL ;
static int noscale = 0 ;
static int transform_loaded = 0 ;
static char *gca_mean_fname = NULL ;
static TRANSFORM  *transform = NULL ;
static char *vf_fname = NULL ;

static double blur_sigma = 0.0f ;

/* 
   command line consists of three inputs:

   argv[1]  - directory containing 'canonical' brain
   argv[2]  - directory containing brain to be registered
   argv[3]  - directory in which to write out registered brain.
*/

#define NPARMS           12
#define DEFAULT_CTL_POINT_PCT   .25
static double ctl_point_pct = DEFAULT_CTL_POINT_PCT ;

int
main(int argc, char *argv[])
{
  char         *gca_fname, *in_fname, *out_fname, fname[STRLEN], **av ;
  MRI          *mri_inputs, *mri_tmp ;
  GCA          *gca /*, *gca_tmp, *gca_reduced*/ ;
  int          ac, nargs, ninputs, input, extra = 0 ;
  int          msec, minutes, seconds /*, iter*/ ;
  struct timeb start ;
  GCA_MORPH    *gcam ;

  parms.l_log_likelihood = 1.0f ;
  parms.niterations = 100 ;
  parms.levels = 5 ;
	parms.relabel_avgs = 1 ;  /* relabel when navgs=1 */
	parms.reset_avgs = 0 ;  /* reset metric properties when navgs=0 */
  parms.dt = 0.05 ;  /* was 5e-6 */
	parms.momentum = 0.9 ;
  parms.tol = .1 ;  /* at least 1% decrease in sse */
  parms.l_distance = 0.0 ;
  parms.l_jacobian = 1.0 ;
  parms.l_area = 0 ;
	parms.l_label = 1.0 ;
	parms.l_map = 0.0 ;
	parms.label_dist = 3.0 ;
  parms.l_smoothness = 2 ;
  parms.start_t = 0 ;
  parms.max_grad = 0.3 ;
  parms.sigma = 2.0f ;
  parms.exp_k = 5 ;
  parms.navgs = 64 ;
  parms.noneg = True ;
  parms.ratio_thresh = 0.125 ;
  parms.nsmall = 1 ;
  parms.integration_type = GCAM_INTEGRATE_BOTH ;
  
  Progname = argv[0] ;
  setRandomSeed(-1L) ;

  DiagInit(NULL, NULL, NULL) ;
  ErrorInit(NULL, NULL, NULL) ;

  ac = argc ;
  av = argv ;
  for ( ; argc > 1 && ISOPTION(*argv[1]) ; argc--, argv++)
  {
    nargs = get_option(argc, argv) ;
    argc -= nargs ;
    argv += nargs ;
  }

  if (argc < 4)
    ErrorExit(ERROR_BADPARM, 
              "usage: %s <in brain> <template> <output file name>\n",
              Progname) ;

  ninputs = argc-3 ;
  printf("reading %d input volumes...\n", ninputs) ;
  in_fname = argv[1] ;
  gca_fname = argv[ninputs+1] ;
  out_fname = argv[ninputs+2] ;
  FileNameOnly(out_fname, fname) ;
  FileNameRemoveExtension(fname, fname) ;
  strcpy(parms.base_name, fname) ;
  Gdiag |= DIAG_WRITE ;
  printf("logging results to %s.log\n", parms.base_name) ;

  TimerStart(&start) ;
  printf("reading GCA '%s'...\n", gca_fname) ;
  fflush(stdout) ;
  gca = GCAread(gca_fname) ;
  if (gca == NULL)
    ErrorExit(ERROR_NOFILE, "%s: could not open GCA %s.\n",
              Progname, gca_fname) ;
  if (map_to_flash)
  {
    GCA *gca_tmp ;
    
    printf("mapping GCA into %d-dimensional FLASH space...\n", mri_inputs->nframes) ;
    gca_tmp = GCAcreateFlashGCAfromParameterGCA(gca, TRs, fas, TEs, mri_inputs->nframes, 100) ;
    GCAfree(&gca) ;
    gca = gca_tmp ;
    if (ninputs != gca->ninputs)
      ErrorExit(ERROR_BADPARM, "%s: must specify %d inputs, not %d for this atlas\n",
		Progname, gca->ninputs, ninputs) ;
    GCAhistoScaleImageIntensities(gca, mri_inputs) ;
    if (novar)
      GCAunifyVariance(gca) ;
  }
  if (gca->type == GCA_FLASH)
  {
    GCA *gca_tmp ;
    
    gca_tmp = GCAcreateFlashGCAfromFlashGCA(gca, TRs, fas, TEs, mri_inputs->nframes) ;
    GCAfree(&gca) ;
    gca = gca_tmp ;
  }
  if (gca->flags & GCA_XGRAD)
    extra += ninputs ;
  if (gca->flags & GCA_YGRAD)
    extra += ninputs ;
  if (gca->flags & GCA_ZGRAD)
    extra += ninputs ;
  
  if ((ninputs+extra) != gca->ninputs)
    ErrorExit(ERROR_BADPARM, "%s: must specify %d inputs, not %d for this atlas\n",
	      Progname, gca->ninputs, ninputs) ;
  
  printf("freeing gibbs priors...") ;
  GCAfreeGibbs(gca) ;
  printf("done.\n") ;

  
  for (input = 0 ; input < ninputs ; input++)
  {
    in_fname = argv[1+input] ;
    printf("reading input volume '%s'...\n", in_fname) ;
    fflush(stdout) ;
    mri_tmp = MRIread(in_fname) ;
    if (!mri_tmp)
    ErrorExit(ERROR_NOFILE, "%s: could not open input volume %s.\n",
              Progname, in_fname) ;
    
    TRs[input] = mri_tmp->tr ;
    fas[input] = mri_tmp->flip_angle ;
    TEs[input] = mri_tmp->te ;
    
    if (mask_fname)
    {
      MRI *mri_mask ;
      
      mri_mask = MRIread(mask_fname) ;
      if (!mri_mask)
	ErrorExit(ERROR_NOFILE, "%s: could not open mask volume %s.\n",
		  Progname, mask_fname) ;
      // if mask == 0, then set dst as 0
      MRImask(mri_tmp, mri_mask, mri_tmp, 0, 0) ;
      MRIfree(&mri_mask) ;
    }
    if (alpha > 0)
      mri_tmp->flip_angle = alpha ;
    if (TR > 0)
      mri_tmp->tr = TR ;
    if (TE > 0)
      mri_tmp->te = TE ;
    if (input == 0)
    {
      mri_inputs = MRIallocSequence(mri_tmp->width, mri_tmp->height, mri_tmp->depth,
				    mri_tmp->type, ninputs+extra) ;
      MRIcopyHeader(mri_tmp, mri_inputs) ;
    }
    MRIcopyFrame(mri_tmp, mri_inputs, 0, input) ;
    MRIfree(&mri_tmp) ;
  }
  
  if (renormalization_fname)
  {
    FILE   *fp ;
    int    *labels, nlines, i ;
    float  *intensities, f1, f2 ;
    char   *cp, line[STRLEN] ;

    fp = fopen(renormalization_fname, "r") ;
    if (!fp)
      ErrorExit(ERROR_NOFILE, "%s: could not read %s",
                Progname, renormalization_fname) ;

    cp = fgetl(line, 199, fp) ;
    nlines = 0 ;
    while (cp)
    {
      nlines++ ;
      cp = fgetl(line, 199, fp) ;
    }
    rewind(fp) ;
    printf("reading %d labels from %s...\n", nlines,renormalization_fname) ;
    labels = (int *)calloc(nlines, sizeof(int)) ;
    intensities = (float *)calloc(nlines, sizeof(float)) ;
    cp = fgetl(line, 199, fp) ;
    for (i = 0 ; i < nlines ; i++)
    {
      sscanf(cp, "%e  %e", &f1, &f2) ;
      labels[i] = (int)f1 ; intensities[i] = f2 ;
      if (labels[i] == Left_Cerebral_White_Matter)
        DiagBreak() ;
      cp = fgetl(line, 199, fp) ;
    }
    GCArenormalizeIntensities(gca, labels, intensities, nlines) ;
    free(labels) ; free(intensities) ;
  }


  if (example_T1)
  {
    MRI *mri_T1, *mri_seg ;

    mri_seg = MRIread(example_segmentation) ;
    if (!mri_seg)
      ErrorExit(ERROR_NOFILE,"%s: could not read example segmentation from %s",
                Progname, example_segmentation) ;
    mri_T1 = MRIread(example_T1) ;
    if (!mri_T1)
      ErrorExit(ERROR_NOFILE,"%s: could not read example T1 from %s",
                Progname, example_T1) ;
    printf("scaling atlas intensities using specified examples...\n") ;
    MRIeraseBorderPlanes(mri_seg) ;
    GCArenormalizeToExample(gca, mri_seg, mri_T1) ;
    MRIfree(&mri_seg) ; MRIfree(&mri_T1) ;
  }

  if (tissue_parms_fname)   /* use FLASH forward model */
    GCArenormalizeToFlash(gca, tissue_parms_fname, mri_inputs) ;

  // transform is loaded at get_opt() with -T using TransformRead()
  // assumed to be vox-to-vox
  if (!transform_loaded)   /* wasn't preloaded */
    transform = TransformAlloc(LINEAR_VOX_TO_VOX, NULL) ;

  if (novar)
    GCAunifyVariance(gca) ;
  if (gca->flags & GCA_GRAD)
  {
    int i, start = ninputs ;
    MRI *mri_kernel, *mri_smooth, *mri_grad, *mri_tmp ;
    
    mri_kernel = MRIgaussian1d(1.0, 30) ;
    mri_smooth = MRIconvolveGaussian(mri_inputs, NULL, mri_kernel) ;
    
    if (mri_inputs->type != MRI_FLOAT)
    {
      mri_tmp = MRISeqchangeType(mri_inputs, MRI_FLOAT, 0, 0, 1) ;
      MRIfree(&mri_inputs) ; mri_inputs = mri_tmp ;
    }
    start = ninputs ;
    if (gca->flags & GCA_XGRAD)
    {
      for (i = 0 ; i < ninputs ; i++)
      {
	mri_grad = MRIxSobel(mri_smooth, NULL, i) ;
	MRIcopyFrame(mri_grad, mri_inputs, 0, start+i) ;
	MRIfree(&mri_grad) ;
      }
      start += ninputs ;
    }
    if (gca->flags & GCA_YGRAD)
    {
      for (i = 0 ; i < ninputs ; i++)
      {
	mri_grad = MRIySobel(mri_smooth, NULL, i) ;
	MRIcopyFrame(mri_grad, mri_inputs, 0, start+i) ;
	MRIfree(&mri_grad) ;
      }
      start += ninputs ;
    }
    if (gca->flags & GCA_ZGRAD)
    {
      for (i = 0 ; i < ninputs ; i++)
      {
	mri_grad = MRIzSobel(mri_smooth, NULL, i) ;
	MRIcopyFrame(mri_grad, mri_inputs, 0, start+i) ;
	MRIfree(&mri_grad) ;
      }
      start += ninputs ;
    }
    
    MRIfree(&mri_kernel) ; MRIfree(&mri_smooth) ; 
  }
  
  if (remove_bright)
    remove_bright_stuff(mri_inputs, gca, transform) ;
  if (!FZERO(blur_sigma))
  {
    MRI *mri_tmp, *mri_kernel ;

    mri_kernel = MRIgaussian1d(blur_sigma, 100) ;
    mri_tmp = MRIconvolveGaussian(mri_inputs, NULL, mri_kernel) ;
    MRIfree(&mri_inputs) ; mri_inputs = mri_tmp ;
  }


  if (transform && Gvx > 0)
  {
    float xf, yf, zf ;
    
    TransformSample(transform, Gvx, Gvy, Gvz, &xf, &yf, &zf) ;
    Gsx = nint(xf) ; Gsy = nint(yf) ; Gsz = nint(zf) ;
    printf("mapping (%d, %d, %d) --> (%d, %d, %d) for rgb writing\n",
	   Gvx, Gvy, Gvz, Gsx, Gsy, Gsz) ;
  }
  
  if (regularize > 0)
    GCAregularizeCovariance(gca, regularize) ;
  if (xform_name)
  {
    gcam = GCAMread(xform_name) ;
    if (!gcam)
      ErrorExit(ERROR_NOFILE, "%s: could not read transform from %s", Progname, xform_name) ;
  }
  else
    gcam = GCAMalloc(gca->prior_width, gca->prior_height, gca->prior_depth) ;
  if (tl_fname)
  {
    GCA *gca_tl ;
    
    gca_tl = GCAread(tl_fname) ;
    if (!gca_tl)
      ErrorExit(ERROR_NOFILE, "%s: could not temporal lobe gca %s",
		Progname, tl_fname) ;
    GCAMinit(gcam, mri_inputs, gca_tl, transform, 0) ;
    if (parms.write_iterations != 0)
    {
      char fname[STRLEN] ;
      MRI  *mri_gca ;
      mri_gca = MRIclone(mri_inputs, NULL) ;
      GCAMbuildMostLikelyVolume(gcam, mri_gca) ;
      sprintf(fname, "%s_target", parms.base_name) ;
      MRIwriteImageViews(mri_gca, fname, IMAGE_SIZE) ;
      sprintf(fname, "%s_target.mgh", parms.base_name) ;
      printf("writing target volume to %s...\n", fname) ;
      MRIwrite(mri_gca, fname) ;
      MRIfree(&mri_gca) ;
    }
    GCAMregister(gcam, mri_inputs, &parms) ;
    printf("temporal lobe registration complete - registering whole brain...\n") ;
    GCAfree(&gca_tl) ;
  }
  
  if (!xform_name)  /* only if the transform wasn't previously created */
    GCAMinit(gcam, mri_inputs, gca, transform, parms.relabel_avgs >= parms.navgs) ;
  else
  {
    gcam->gca = gca ;
    GCAMcomputeOriginalProperties(gcam) ;
    if (parms.relabel_avgs >= parms.navgs)
      GCAMcomputeLabels(mri_inputs, gcam) ;
    else
      GCAMcomputeMaxPriorLabels(gcam) ;
  }
#if 0
  {
    GCA_SAMPLE *gcas ;
    int  nsamples ;

    gcas = GCAfindAllSamples(gca, &nsamples) ;
    GCAtransformAndWriteSamples(gca, mri_inputs, gcas, nsamples, 
                                "gcas_fsamples.mgh", transform) ;
    free(gcas) ;
  }
#endif

  if (tl_fname == NULL && register_wm_flag)
  {
    GCAMsetStatus(gcam, GCAM_IGNORE_LIKELIHOOD) ; /* disable everything */
    GCAMsetLabelStatus(gcam, Left_Cerebral_White_Matter, GCAM_USE_LIKELIHOOD) ;
    GCAMsetLabelStatus(gcam, Right_Cerebral_White_Matter, GCAM_USE_LIKELIHOOD) ;
    GCAMsetLabelStatus(gcam, Left_Cerebellum_White_Matter, GCAM_USE_LIKELIHOOD) ;
    GCAMsetLabelStatus(gcam, Right_Cerebellum_White_Matter, GCAM_USE_LIKELIHOOD) ;
    
    printf("initial white matter registration...\n") ;
    GCAMregister(gcam, mri_inputs, &parms) ;
    GCAMsetStatus(gcam, GCAM_USE_LIKELIHOOD) ; /* disable everything */
    printf("initial white matter registration complete - full registration...\n") ;
  }
  if (renormalize)
    GCAmapRenormalize(gcam->gca, mri_inputs, transform) ;

  if (parms.write_iterations != 0)
  {
    char fname[STRLEN] ;
    MRI  *mri_gca ;
    mri_gca = MRIclone(mri_inputs, NULL) ;
    GCAMbuildMostLikelyVolume(gcam, mri_gca) ;
    sprintf(fname, "%s_target", parms.base_name) ;
    MRIwriteImageViews(mri_gca, fname, IMAGE_SIZE) ;
    sprintf(fname, "%s_target.mgh", parms.base_name) ;
    printf("writing target volume to %s...\n", fname) ;
    MRIwrite(mri_gca, fname) ;
    MRIfree(&mri_gca) ;
  }

  if (reset)
  {
    GCAMcopyNodePositions(gcam, CURRENT_POSITIONS, ORIGINAL_POSITIONS) ;
    GCAMstoreMetricProperties(gcam) ;
  }
  GCAMregister(gcam, mri_inputs, &parms) ;
  
#if 0
  for (iter = 0 ; iter < 3 ; iter++)
  {
    parms.relabel_avgs = 1 ;
    GCAMcopyNodePositions(gcam, CURRENT_POSITIONS, ORIGINAL_POSITIONS) ;
    GCAMstoreMetricProperties(gcam) ;
    parms.levels = 2 ;
    parms.navgs = 1 ;
    GCAMregister(gcam, mri_inputs, &parms) ;
  }
#endif

#if 0
  parms.l_distance = 0 ;
  parms.relabel = 1 ;
  GCAMcomputeLabels(mri_inputs, gcam) ;
  GCAMregister(gcam, mri_inputs, &parms) ;
#endif

  printf("writing output transformation to %s...\n", out_fname) ;
  if (vf_fname)
    write_vector_field(mri_inputs, gcam, vf_fname) ;
  if (GCAMwrite(gcam, out_fname) != NO_ERROR)
    ErrorExit(Gerror, "%s: GCAMwrite(%s) failed", Progname, out_fname) ;

#if 0
  if (gca)
    GCAfree(&gca) ;
#endif
  GCAMfree(&gcam) ;
  if (mri_inputs)
    MRIfree(&mri_inputs) ;
  msec = TimerStop(&start) ;
  seconds = nint((float)msec/1000.0f) ;
  minutes = seconds / 60 ;
  seconds = seconds % 60 ;
  printf("registration took %d minutes and %d seconds.\n", 
          minutes, seconds) ;
  if (diag_fp)
    fclose(diag_fp) ;
  exit(0) ;
  return(0) ;
}



/*----------------------------------------------------------------------
            Parameters:

           Description:
----------------------------------------------------------------------*/
static int
get_option(int argc, char *argv[])
{
  int  nargs = 0 ;
  char *option ;

  option = argv[1] + 1 ;            /* past '-' */
	/*  StrUpper(option) ;*/
  if (!stricmp(option, "DIST") || !stricmp(option, "DISTANCE"))
  {
    parms.l_distance = atof(argv[2]) ;
    nargs = 1 ;
    printf("l_dist = %2.2f\n", parms.l_distance) ;
  }
  else if (!stricmp(option, "REGULARIZE"))
  {
    regularize = atof(argv[2]) ;
    printf("regularizing variance to be sigma+%2.1fC(noise)\n", regularize) ;
		nargs = 1 ;
  }
  else if (!stricmp(option, "NOBRIGHT"))
  {
    remove_bright = 1 ;
    printf("removing bright non-brain structures...\n") ;
  }
  else if (!stricmp(option, "renormalize"))
  {
    renormalize = 1 ;
    printf("renormalizing GCA to MAP estimate of means\n") ;
  }
  else if (!stricmp(option, "SMOOTH") || !stricmp(option, "SMOOTHNESS"))
  {
    parms.l_smoothness = atof(argv[2]) ;
    nargs = 1 ;
    printf("l_smoothness = %2.2f\n", parms.l_smoothness) ;
  }
  else if (!stricmp(option, "SAMPLES"))
  {
    sample_fname = argv[2] ;
    nargs = 1 ;
    printf("writing control points to %s...\n", sample_fname) ;
  }
  else if (!stricmp(option, "SMALL") || !stricmp(option, "NSMALL"))
  {
    parms.nsmall = atoi(argv[2]) ;
    nargs = 1 ;
    printf("allowing %d small steps before terminating integration\n",
			parms.nsmall) ;
  }
  else if (!stricmp(option, "FIXED"))
  {
    parms.integration_type = GCAM_INTEGRATE_FIXED ;
    printf("using fixed time-step integration\n") ;
  }
  else if (!stricmp(option, "OPTIMAL"))
  {
    parms.integration_type = GCAM_INTEGRATE_OPTIMAL ;
    printf("using optimal time-step integration\n") ;
  }
  else if (!stricmp(option, "NONEG"))
  {
    parms.noneg = atoi(argv[2]) ;
    nargs = 1 ;
    printf("%s allowing temporary folds during numerical minimization\n",
					 parms.noneg ? "not" : "") ;
  }
  else if (!stricmp(option, "ISIZE") || !stricmp(option, "IMAGE_SIZE"))
  {
    IMAGE_SIZE = atoi(argv[2]) ;
    nargs = 1 ;
    printf("setting diagnostic image size to %d\n", IMAGE_SIZE) ;
  }
  else if (!stricmp(option, "WM"))
  {
    register_wm_flag = 1 ;
    printf("registering white matter in initial pass...\n") ;
  }
  else if (!stricmp(option, "TL"))
  {
    tl_fname = argv[2] ;
    nargs = 1 ;
    printf("reading temporal lobe atlas from %s...\n", tl_fname) ;
  }
  else if (!stricmp(option, "RELABEL"))
  {
    parms.relabel = atoi(argv[2]) ;
    nargs = 1 ;
    printf("%srelabeling nodes with MAP estimates\n", 
					 parms.relabel ? "" : "not ") ;
  }
  else if (!stricmp(option, "RELABEL_AVGS"))
  {
    parms.relabel_avgs = atoi(argv[2]) ;
    nargs = 1 ;
    printf("relabeling nodes with MAP estimates at avgs=%d\n", parms.relabel_avgs) ;
  }
  else if (!stricmp(option, "RESET_AVGS"))
  {
    parms.reset_avgs = atoi(argv[2]) ;
    nargs = 1 ;
    printf("resetting metric properties at avgs=%d\n", parms.reset_avgs) ;
  }
  else if (!stricmp(option, "RESET"))
  {
		reset = 1 ;
    printf("resetting metric properties...\n") ;
  }
  else if (!stricmp(option, "VF"))
  {
    vf_fname = argv[2] ;
    nargs = 1 ;
    printf("writing vector field to %s...\n", vf_fname) ;
  }
  else if (!stricmp(option, "MASK"))
  {
    mask_fname = argv[2] ;
    nargs = 1 ;
    printf("using MR volume %s to mask input volume...\n", mask_fname) ;
  }
  else if (!stricmp(option, "DIAG"))
  {
    diag_fp = fopen(argv[2], "w") ;
    if (!diag_fp)
      ErrorExit(ERROR_NOFILE, "%s: could not open diag file %s for writing",
                Progname, argv[2]) ;
    printf("opening diag file %s for writing\n", argv[2]) ;
    nargs = 1 ;
  }
  else if (!stricmp(option, "TR"))
  {
    TR = atof(argv[2]) ;
    nargs = 1 ;
    printf("using TR=%2.1f msec\n", TR) ;
  }
  else if (!stricmp(option, "EXAMPLE"))
  {
    example_T1 = argv[2] ;
    example_segmentation = argv[3] ;
    printf("using %s and %s as example T1 and segmentations respectively.\n",
           example_T1, example_segmentation) ;
    nargs = 2 ;
  }
  else if (!stricmp(option, "TE"))
  {
    TE = atof(argv[2]) ;
    nargs = 1 ;
    printf("using TE=%2.1f msec\n", TE) ;
  }
  else if (!stricmp(option, "ALPHA"))
  {
    nargs = 1 ;
    alpha = RADIANS(atof(argv[2])) ;
    printf("using alpha=%2.0f degrees\n", DEGREES(alpha)) ;
  }
  else if (!stricmp(option, "FSAMPLES") || !stricmp(option, "ISAMPLES"))
  {
    transformed_sample_fname = argv[2] ;
    nargs = 1 ;
    printf("writing transformed control points to %s...\n", 
            transformed_sample_fname) ;
  }
  else if (!stricmp(option, "NSAMPLES"))
  {
    normalized_transformed_sample_fname = argv[2] ;
    nargs = 1 ;
    printf("writing  transformed normalization control points to %s...\n", 
            normalized_transformed_sample_fname) ;
  }
  else if (!stricmp(option, "CONTRAST"))
  {
    use_contrast = 1 ;
    printf("using contrast to find labels...\n") ;
  }
  else if (!stricmp(option, "RENORM"))
  {
    renormalization_fname = argv[2] ;
    nargs = 1 ;
    printf("renormalizing using predicted intensity values in %s...\n",
           renormalization_fname) ;
  }
  else if (!stricmp(option, "FLASH"))
  {
		map_to_flash = 1 ;
    printf("using FLASH forward model to predict intensity values...\n") ;
  }
  else if (!stricmp(option, "FLASH_PARMS"))
  {
    tissue_parms_fname = argv[2] ;
    nargs = 1 ;
    printf("using FLASH forward model and tissue parms in %s to predict"
           " intensity values...\n", tissue_parms_fname) ;
  }
  else if (!stricmp(option, "TRANSONLY"))
  {
    translation_only = 1 ;
    printf("only computing translation parameters...\n") ;
  }
  else if (!stricmp(option, "WRITE_MEAN"))
  {
    gca_mean_fname = argv[2] ;
    nargs = 1 ;
    printf("writing gca means to %s...\n", gca_mean_fname) ;
  }
  else if (!stricmp(option, "PRIOR"))
  {
    min_prior = atof(argv[2]) ;
    nargs = 1 ;
    printf("using prior threshold %2.2f\n", min_prior) ;
  }
  else if (!stricmp(option, "NOVAR"))
  {
    novar = 1 ;
    printf("not using variance estimates\n") ;
  }
  else if (!stricmp(option, "USEVAR"))
  {
    novar = 0 ;
    printf("using variance estimates\n") ;
  }
  else if (!stricmp(option, "DT"))
  {
    parms.dt = atof(argv[2]) ;
    nargs = 1 ;
    printf("dt = %2.2e\n", parms.dt) ;
  }
  else if (!stricmp(option, "TOL"))
  {
    parms.tol = atof(argv[2]) ;
    nargs = 1 ;
    printf("tol = %2.2e\n", parms.tol) ;
  }
  else if (!stricmp(option, "CENTER"))
  {
    center = 1 ;
    printf("using GCA centroid as origin of transform\n") ;
  }
  else if (!stricmp(option, "NOSCALE"))
  {
    noscale = 1 ;
    printf("disabling scaling...\n") ;
  }
  else if (!stricmp(option, "LEVELS"))
  {
    parms.levels = atoi(argv[2]) ;
    nargs = 1 ;
    printf("levels = %d\n", parms.levels) ;
  }
  else if (!stricmp(option, "LIKELIHOOD"))
  {
    parms.l_likelihood = atof(argv[2]) ;
    nargs = 1 ;
    printf("l_likelihood = %2.2f\n", parms.l_likelihood) ;
  }
  else if (!stricmp(option, "LOGLIKELIHOOD"))
  {
    parms.l_log_likelihood = atof(argv[2]) ;
    nargs = 1 ;
    printf("l_log_likelihood = %2.2f\n", parms.l_log_likelihood) ;
  }
  else if (!stricmp(option, "LABEL"))
  {
    parms.l_label = atof(argv[2]) ;
    nargs = 1 ;
    printf("l_label = %2.2f\n", parms.l_label) ;
  }
  else if (!stricmp(option, "MAP"))
  {
    parms.l_map = atof(argv[2]) ;
    nargs = 1 ;
    printf("l_map = %2.2f\n", parms.l_map) ;
  }
  else if (!stricmp(option, "LDIST") || !stricmp(option, "LABEL_DIST"))
  {
    parms.label_dist = atof(argv[2]) ;
    nargs = 1 ;
    printf("label_dist = %2.2f\n", parms.label_dist) ;
  }
  else if (!stricmp(option, "REDUCE"))
  {
    nreductions = atoi(argv[2]) ;
    nargs = 1 ;
    printf("reducing input images %d times before aligning...\n",
            nreductions) ;
  }
  else if (!stricmp(option, "DEBUG_NODE"))
  {
    Gx = atoi(argv[2]) ;
    Gy = atoi(argv[3]) ;
    Gz = atoi(argv[4]) ;
    nargs = 3 ;
    printf("debugging node (%d, %d, %d)\n", Gx, Gy, Gz) ;
  }
  else if (!stricmp(option, "DEBUG_VOXEL"))
  {
    Gvx = atoi(argv[2]) ;
    Gvy = atoi(argv[3]) ;
    Gvz = atoi(argv[4]) ;
    nargs = 3 ;
    printf("debugging voxel (%d, %d, %d)\n", Gvx, Gvy, Gvz) ;
  }
  else if (!stricmp(option, "norm"))
  {
    norm_fname = argv[2] ;
    nargs = 1 ;
    printf("intensity normalizing and writing to %s...\n",norm_fname);
  }
  else if (!stricmp(option, "avgs"))
  {
		avgs = atoi(argv[2]) ;
    nargs = 1 ;
    fprintf(stderr, "applying mean filter %d times to conditional densities...\n", avgs) ;
  }
  else if (!stricmp(option, "cross-sequence") || !stricmp(option, "cross_sequence"))
  {
    regularize = .5 ;
		avgs = 2 ;
		renormalize = 1 ;
    printf("registering sequences, equivalent to:\n") ;
		printf("\t-renormalize\n\t-avgs %d\n\t-regularize %2.3f\n",avgs, regularize) ;
  } 
  else if (!stricmp(option, "area"))
  {
    parms.l_area = atof(argv[2]) ;
    nargs = 1 ;
    printf("using l_area=%2.3f\n", parms.l_area) ;
  }
 else if (!stricmp(option, "rthresh"))
  {
		parms.ratio_thresh = atof(argv[2]) ;
    nargs = 1 ;
    printf("using compression ratio threshold = %2.3f...\n", parms.ratio_thresh) ;
  }
  else switch (toupper(*option))
  {
  case 'J':
    parms.l_jacobian = atof(argv[2]) ;
    nargs = 1 ;
    printf("using l_jacobian=%2.3f\n", parms.l_jacobian) ;
    break ;
  case 'A':
		parms.navgs = atoi(argv[2]) ;
    nargs = 1 ;
    printf("smoothing gradient with %d averages...\n", parms.navgs) ;
    break ;
  case 'F':
    ctl_point_fname = argv[2] ;
    nargs = 1 ;
    printf("reading manually defined control points from %s\n", ctl_point_fname) ;
    break ;
	case 'X':
		xform_name = argv[2] ;
		nargs = 1 ;
		printf("reading previous transform from %s...\n", xform_name) ;
		break ;
  case 'K':
    parms.exp_k = atof(argv[2]) ;
    printf("setting exp_k to %2.2f (default=%2.2f)\n",
           parms.exp_k, EXP_K) ;
    nargs = 1 ;
    break ;
  case 'T':
    transform = TransformRead(argv[2]) ;
    if (!transform)
      ErrorExit(ERROR_BADFILE, "%s: could not read transform file %s",
                Progname, argv[2]) ;
    if (transform->type != LINEAR_VOX_TO_VOX)
      ErrorExit(ERROR_BADPARM, "%s: does not have VOX_TO_VOX transform %s\n",
		Progname, argv[2]);
    nargs = 1 ;
    printf("using previously computed transform %s\n", argv[2]) ;
    transform_loaded = 1 ;
    break ;
  case 'B':
    blur_sigma = atof(argv[2]) ;
    nargs = 1 ;
    printf("blurring input image with sigma=%2.3f\n", blur_sigma);
    break ;
  case 'V':
    Gdiag_no = atoi(argv[2]) ;
    nargs = 1 ;
    break ;
  case 'S':
    parms.sigma = atof(argv[2]) ;
    printf("using sigma=%2.3f as upper bound on blurring.\n", 
            parms.sigma) ;
    nargs = 1 ;
    break ;
  case '?':
  case 'U':
    printf("usage: %s <in volume> <template volume> <output transform>\n", 
           argv[0]) ;
    exit(1) ;
    break ;
  case 'N':
    parms.niterations = atoi(argv[2]) ;
    nargs = 1 ;
    printf("niterations = %d\n", parms.niterations) ;
    break ;
  case 'W':
    parms.write_iterations = atoi(argv[2]) ;
    nargs = 1 ;
    printf("write iterations = %d\n", parms.write_iterations) ;
    Gdiag |= DIAG_WRITE ;
    break ;
  case 'P':
    ctl_point_pct = atof(argv[2]) ;
    nargs = 1 ;
    printf("using top %2.1f%% wm points as control points....\n",
           100.0*ctl_point_pct) ;
    break ;
  case 'M':
    parms.momentum = atof(argv[2]) ;
    nargs = 1 ;
    printf("momentum = %2.2f\n", parms.momentum) ;
    break ;
  default:
    printf("unknown option %s\n", argv[1]) ;
    exit(1) ;
    break ;
  }

  return(nargs) ;
}
static int
write_vector_field(MRI *mri, GCA_MORPH *gcam, char *vf_fname)
{
  FILE            *fp ;
  int             x, y, z ;
  GCA_MORPH_NODE  *gcamn ;

  fp = fopen(vf_fname, "w") ;

  for (x = 0 ; x < gcam->width ; x++)
  {
    for (y = 0 ; y < gcam->height ; y++)
    {
      for (z = 0 ; z < gcam->depth ; z++)
      {
        if (x == Gx && y == Gy && z == Gz)
          DiagBreak() ;
        gcamn = &gcam->nodes[x][y][z] ;
        fprintf(fp, "%f %f %f %f\n", 
                gcamn->x-gcamn->origx,
                gcamn->y-gcamn->origy,
                gcamn->z-gcamn->origz, 
                gcamn->gc ? gcamn->gc->means[0] : 0.0) ;
      }
    }
  }
  fclose(fp) ;
  return(NO_ERROR) ;
}

static int
remove_bright_stuff(MRI *mri, GCA *gca, TRANSFORM *transform)
{
  HISTO            *h, *hs ;
  int              peak, num, end, x, y, z, xi, yi, zi, xk, yk, zk, i, n, erase, five_mm ;
  float            thresh ;
  Real             val, new_val ;
  MRI              *mri_tmp, *mri_nonbrain, *mri_tmp2 ;
  GCA_PRIOR        *gcap ;
  MRI_SEGMENTATION *mriseg ;
  MRI_SEGMENT      *mseg ;
  MSV              *msv ;
  
  
  if (gca->ninputs > 1)
    return(NO_ERROR) ;
  
  mri_tmp = MRIalloc(mri->width, mri->height, mri->depth, MRI_UCHAR) ;
  mri_nonbrain = MRIalloc(mri->width, mri->height, mri->depth, MRI_UCHAR) ;
  for (x = 0 ; x < mri->width ; x++)
  {
    for (y = 0 ; y < mri->height ; y++)
    {
      for (z = 0 ; z < mri->depth ; z++)
      {
	gcap = getGCAP(gca, mri, transform, x, y, z) ;
	if (gcap->nlabels == 0 || (gcap->nlabels == 1 && IS_UNKNOWN(gcap->labels[0])))
	  MRIvox(mri_nonbrain, x, y, z) = 1 ;
	else
	{
	  MRIsampleVolume(mri, x, y, z, &val) ;
	  if (FZERO(val))
	    MRIvox(mri_nonbrain, x, y, z) = 128 ;
	}
      }
    }
  }
  /* dilate it by 0.5 cm */
  five_mm = nint(5.0*pow(mri->xsize*mri->ysize*mri->zsize, 1.0f/3.0f)) ;
  for (i = 0 ; i < five_mm ; i++)
  {
    MRIdilate(mri_nonbrain, mri_tmp) ;
    MRIcopy(mri_tmp, mri_nonbrain) ;
  }
  
  MRIclear(mri_tmp) ;
  h = MRIhistogram(mri, 0) ;
  h->counts[0] = 0 ;
  hs = HISTOsmooth(h, NULL, 2) ;
  
  peak = HISTOfindLastPeak(hs, 5, 0.1) ;
  end = HISTOfindEndOfPeak(hs, peak, 0.01) ;
  thresh = hs->bins[end] ;
  new_val = 0 ;
  
  printf("removing voxels brighter than %2.1f\n", thresh) ;
  
  for (num = x = 0 ; x < mri->width ; x++)
  {
    for (y = 0 ; y < mri->height ; y++)
    {
      for (z = 0 ; z < mri->depth ; z++)
      {
	if (x == Gvx && y == Gvy && z == Gvz)
	  DiagBreak() ;
	MRIsampleVolume(mri, x, y, z, &val) ;
	if (val > thresh)
	{
	  num++ ;
	  MRIvox(mri_tmp, x, y, z) = 128 ;
	  /*					MRIsetVoxVal(mri, x, y, z, 0, (float)new_val) ;*/
	}
      }
    }
  }
  
  
  /* relax threshold somewhat, and reduce voxels that are above this thresh
     and nbrs of one above the more stringent one.
  */
  end = HISTOfindStartOfPeak(hs, peak, 0.1) ;
  thresh = hs->bins[end] ;
  mri_tmp2 = MRIcopy(mri_tmp, NULL) ;
  for (x = 0 ; x < mri->width ; x++)
  {
    for (y = 0 ; y < mri->height ; y++)
    {
      for (z = 0 ; z < mri->depth ; z++)
      {
	if (MRIvox(mri_tmp2, x, y, z) == 0)
	  continue ;
	for (xk = -1 ; xk <= 1 ; xk++)
	{
	  xi = mri_tmp->xi[x+xk] ;
	  for (yk = -1 ; yk <= 1 ; yk++)
	  {
	    yi = mri_tmp->yi[y+yk] ;
	    for (zk = -1 ; zk <= 1 ; zk++)
	    {
	      zi = mri_tmp->zi[z+zk] ;
	      if (xi == Gvx && yi == Gvy && zi == Gvz)
		DiagBreak() ;
	      MRIsampleVolume(mri, xi, yi, zi, &val) ;
	      if (val > thresh)
	      {
		num++ ;
		MRIvox(mri_tmp, xi, yi, zi) = 128 ;
		/*								MRIsetVoxVal(mri, xi, yi, zi, 0, (float)new_val) ;*/
	      }
	    }
	  }
	}
      }
    }
  }
  
  MRIfree(&mri_tmp2) ;
  mriseg = MRIsegment(mri_tmp, 1, 255) ;
  printf("%d bright voxels found - %d segments\n", num, mriseg->nsegments) ;
  
  
  for (num = i = 0 ; i < mriseg->nsegments ; i++)
  {
    /* check to see that at least one voxel in segment is in nonbrain mask (i.e. it is within 1cm of
       nonbrain */
    mseg = &mriseg->segments[i] ;
    for (erase = 0, n = 0 ; n < mseg->nvoxels ; n++)
    {
      msv = &mseg->voxels[n] ;
      if (msv->x == Gvx && msv->y == Gvy && msv->z == Gvz)
	DiagBreak() ;
      if (MRIvox(mri_nonbrain, msv->x, msv->y, msv->z) > 0)
      {
	erase = 1 ;
	break ;
      }
    }
    if (erase)
    {
      if (DIAG_VERBOSE_ON)
	printf("erasing segment %d (%d voxels) with centroid at (%2.0f, %2.0f, %2.0f)\n",
	       i, mseg->nvoxels, mseg->cx, mseg->cy, mseg->cz) ;
      for (n = 0 ; n < mseg->nvoxels ; n++)
      {
	msv = &mseg->voxels[n] ;
	if (msv->x == Gx && msv->y == Gy && msv->z == Gz)
	  DiagBreak() ;
	MRIsetVoxVal(mri, msv->x, msv->y, msv->z, 0, 0.0f) ;
	num++ ;
      }
    }
  }
  
  printf("%d bright voxels erased\n", num) ;
  HISTOfree(&h) ; HISTOfree(&hs) ; MRIfree(&mri_tmp) ; MRIfree(&mri_nonbrain) ;
  MRIsegmentFree(&mriseg) ;
  return(NO_ERROR) ;
}

