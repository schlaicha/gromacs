/* -*- mode: c; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; c-file-style: "stroustrup"; -*-
 *
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2010, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Gallium Rubidium Oxygen Manganese Argon Carbon Silicon
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gmx_fatal.h"
#include "typedefs.h"
#include "macros.h"
#include "network.h"
#include "statutil.h"
#include "gmx_omp.h"
#include "gmx_omp_nthreads.h"
#include "md_logging.h"

/** Structure with the number of threads for each OpenMP multi-threaded
 *  algorithmic module in mdrun. */
typedef struct
{
    int gnth;               /**< Global num. of threads per PP or PP+PME process/tMPI thread. */
    int gnth_pme;           /**< Global num. of threads per PME only process/tMPI thread. */

    int nth[emntNR];        /**< Number of threads for each module, indexed with module_nth_t */
    gmx_bool initialized;   /**< TRUE if the module as been initialized. */
} omp_module_nthreads_t;

/** Names of environment variables to set the per module number of threads.
 *
 *  Indexed with the values of module_nth_t.
 * */
static const char *modth_env_var[emntNR] =
{
    "GMX_DEFAULT_NUM_THREADS should never be set",
    "GMX_DOMDEC_NUM_THREADS", "GMX_PAIRSEARCH_NUM_THREADS",
    "GMX_NONBONDED_NUM_THREADS", "GMX_BONDED_NUM_THREADS",
    "GMX_PME_NUM_THREADS", "GMX_UPDATE_NUM_THREADS",
    "GMX_LINCS_NUM_THREADS", "GMX_SETTLE_NUM_THREADS"
};

/** Names of the modules. */
static const char *mod_name[emntNR] =
{
    "default", "domain decomposition", "pair search", "non-bonded",
    "bonded", "PME", "update", "LINCS", "SETTLE"
};

/** Number of threads for each algorithmic module.
 *
 *  File-scope global variable that gets set once in \init_module_nthreads
 *  and queried via gmx_omp_nthreads_get.
 *
 *  All fields are initialized to 0 which should result in errors if
 *  the init call is omitted.
 * */
static omp_module_nthreads_t modth = { 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}, FALSE};


/** Determine the number of threads for module \mod.
 *
 *  \m takes values form the module_nth_t enum and maps these to the
 *  corresponding value in modth_env_var.
 *
 *  Each number of threads per module takes the default value unless
 *  GMX_*_NUM_THERADS env var is set, case in which its value overrides
 *  the deafult.
 *
 *  The "group" scheme supports OpenMP only in PME and in thise case all but
 *  the PME nthread values default to 1.
 */
static int pick_module_nthreads(FILE *fplog, int m,
                                gmx_bool bSimMaster,
                                gmx_bool bFullOmpSupport,
                                gmx_bool bSepPME)
{
    char *env;
    int  nth;
    char sbuf[STRLEN];
    gmx_bool bOMP;

#ifdef GMX_OPENMP
    bOMP = TRUE;
#else
    bOMP = FALSE;
#endif /* GMX_OPENMP */

    /* The default should never be set through a GMX_*_NUM_THREADS env var
     * as it's always equal with gnth. */
    if (m == emntDefault)
    {
        return modth.nth[emntDefault];
    }

    /* check the environment variable */
    if ((env = getenv(modth_env_var[m])) != NULL)
    {
        sscanf(env, "%d", &nth);

        if (!bOMP)
        {
            gmx_warning("%s=%d is set, but %s is compiled without OpenMP!",
                        modth_env_var[m], nth, ShortProgram());
        }

        /* with the verlet codepath, when any GMX_*_NUM_THREADS env var is set,
         * OMP_NUM_THREADS also has to be set */
        if (bFullOmpSupport && getenv("OMP_NUM_THREADS") == NULL)
        {
            gmx_fatal(FARGS, "%s=%d is set, the default number of threads also "
                      "needs to be set with OMP_NUM_THREADS!",
                      modth_env_var[m], nth);
        }

        /* with the group scheme warn if any env var except PME is set */
        if (!bFullOmpSupport)
        {
            if (m != emntPME)
            {
                gmx_warning("%s=%d is set, but OpenMP multithreading is not "
                            "supported in %s!",
                            modth_env_var[m], nth, mod_name[m]);
                nth = 1;
            }
        }

        /* only babble if we are really overriding with a different value */
        if ((bSepPME && m == emntPME && nth != modth.gnth_pme) || (nth != modth.gnth))
        {
            sprintf(sbuf, "%s=%d set, overriding the default number of %s threads",
                    modth_env_var[m], nth, mod_name[m]);
            if (bSimMaster)
            {
                fprintf(stderr, "\n%s\n", sbuf);
            }
            if (fplog)
            {
                fprintf(fplog, "%s\n", sbuf);
            }
        }
    }
    else
    {
        /* pick the global PME node nthreads if we are setting the number
         * of threads in separate PME nodes  */
        nth = (bSepPME && m == emntPME) ? modth.gnth_pme : modth.gnth;
    }

    return modth.nth[m] = nth;
}

void gmx_omp_nthreads_read_env(int *nthreads_omp)
{
    char *env;

    assert(nthreads_omp);

    if ((env = getenv("OMP_NUM_THREADS")) != NULL)
    {
        int nt_omp;

        sscanf(env,"%d",&nt_omp);
        if (nt_omp <= 0)
        {
            gmx_fatal(FARGS,"OMP_NUM_THREADS is invalid: '%s'",env);
        }

        if (*nthreads_omp > 0 && nt_omp != *nthreads_omp)
        {
            gmx_fatal(FARGS,"OMP_NUM_THREADS (%d) and the number of threads requested on the command line (%d) have different values",nt_omp,*nthreads_omp);
        }

        /* Setting the number of OpenMP threads.
         * NOTE: with tMPI this function is only called on the master node,
         * but with MPI on all nodes which means lots of messages on stderr.
         */
        fprintf(stderr,"Getting the number of OpenMP threads from OMP_NUM_THREADS: %d\n",nt_omp);
        *nthreads_omp = nt_omp;
    }
}

void gmx_omp_nthreads_init(FILE *fplog, t_commrec *cr,
                           int nthreads_hw_avail,
                           int omp_nthreads_req,
                           int omp_nthreads_pme_req,
                           gmx_bool bThisNodePMEOnly,
                           gmx_bool bFullOmpSupport)
{
    int  nth, nth_pmeonly, gmx_maxth, nppn;
    char *env;
    gmx_bool bSepPME, bOMP;

#ifdef GMX_OPENMP
    bOMP = TRUE;
#else
    bOMP = FALSE;
#endif /* GMX_OPENMP */

    /* number of processes per node */
    nppn = cr->nnodes_intra;

    bSepPME = ( (cr->duty & DUTY_PP) && !(cr->duty & DUTY_PME)) ||
              (!(cr->duty & DUTY_PP) &&  (cr->duty & DUTY_PME));

#ifdef GMX_THREAD_MPI
    /* modth is shared among tMPI threads, so for thread safety do the
     * detection is done on the master only. It is not thread-safe with
     * multiple simulations, but that's anyway not supported by tMPI. */
    if (SIMMASTER(cr))
#endif
    {
        /* just return if the initialization has already been done */
        if (modth.initialized)
        {
            return;
        }

        /* With full OpenMP support (verlet scheme) set the number of threads
         * per process / default:
         * - 1 if not compiled with OpenMP or
         * - OMP_NUM_THREADS if the env. var is set, or
         * - omp_nthreads_req = #of threads requested by the user on the mdrun
         *   command line, otherwise
         * - take the max number of available threads and distribute them
         *   on the processes/tMPI threads.
         * ~ The GMX_*_NUM_THREADS env var overrides the number of threads of
         *   the respective module and it has to be used in conjunction with
         *   OMP_NUM_THREADS.
         *
         * With the group scheme OpenMP multithreading is only supported in PME,
         * for all other modules nthreads is set to 1.
         * The number of PME threads is equal to:
         * - 1 if not compiled with OpenMP or
         * - GMX_PME_NUM_THREADS if defined, otherwise
         * - OMP_NUM_THREADS if defined, otherwise
         * - 1
         */
        nth = 1;
        if ((env = getenv("OMP_NUM_THREADS")) != NULL)
        {
            if (!bOMP && (strncmp(env, "1", 1) != 0))
            {
                gmx_warning("OMP_NUM_THREADS is set, but %s was compiled without OpenMP support!",
                            ShortProgram());
            }
            else
            {
                nth = gmx_omp_get_max_threads();
            }
        }
        else if (omp_nthreads_req > 0)
        {
            nth = omp_nthreads_req;
        }
        else if (bFullOmpSupport && bOMP)
        {
            /* max available threads per node */
            nth = nthreads_hw_avail;

            /* divide the threads among the MPI processes/tMPI threads */
            if (nth >= nppn)
            {
                nth /= nppn;
            }
            else
            {
                nth = 1;
            }
        }

        /* now we have the global values, set them:
         * - 1 if not compiled with OpenMP and for the group scheme
         * - nth for the verlet scheme when compiled with OpenMP
         */
        if (bFullOmpSupport && bOMP)
        {
            modth.gnth = nth;
        }
        else
        {
            modth.gnth = 1;
        }

        if (bSepPME)
        {
            if (omp_nthreads_pme_req > 0)
            {
                modth.gnth_pme = omp_nthreads_pme_req;
            }
            else
            {
                modth.gnth_pme = nth;
            }
        }
        else
        {
            modth.gnth_pme = 0;
        }

        /* now set the per-module values */
        modth.nth[emntDefault] = modth.gnth;
        pick_module_nthreads(fplog, emntDomdec, SIMMASTER(cr), bFullOmpSupport, bSepPME);
        pick_module_nthreads(fplog, emntPairsearch, SIMMASTER(cr), bFullOmpSupport, bSepPME);
        pick_module_nthreads(fplog, emntNonbonded, SIMMASTER(cr), bFullOmpSupport, bSepPME);
        pick_module_nthreads(fplog, emntBonded, SIMMASTER(cr), bFullOmpSupport, bSepPME);
        pick_module_nthreads(fplog, emntPME, SIMMASTER(cr), bFullOmpSupport, bSepPME);
        pick_module_nthreads(fplog, emntUpdate, SIMMASTER(cr), bFullOmpSupport, bSepPME);
        pick_module_nthreads(fplog, emntLINCS, SIMMASTER(cr), bFullOmpSupport, bSepPME);
        pick_module_nthreads(fplog, emntSETTLE, SIMMASTER(cr), bFullOmpSupport, bSepPME);

        /* set the number of threads globally */
        if (bOMP)
        {
#ifndef GMX_THREAD_MPI
            if (bThisNodePMEOnly)
            {
                gmx_omp_set_num_threads(modth.gnth_pme);
            }
            else
#endif /* GMX_THREAD_MPI */
            {
                if (bFullOmpSupport)
                {
                    gmx_omp_set_num_threads(nth);
                }
                else
                {
                    gmx_omp_set_num_threads(1);
                }
            }
        }

        modth.initialized = TRUE;
    }
#ifdef GMX_THREAD_MPI
    /* Non-master threads have to wait for the detection to be done. */
    if (PAR(cr))
    {
        MPI_Barrier(cr->mpi_comm_mysim);
    }
#endif

    /* inform the user about the settings */
    if (SIMMASTER(cr) && bOMP)
    {
#ifdef GMX_THREAD_MPI
        const char *mpi_str="per tMPI thread";
#else
        const char *mpi_str="per MPI process";
#endif

        /* for group scheme we print PME threads info only */
        if (bFullOmpSupport)
        {
            fprintf(stderr, "Using %d OpenMP thread%s %s\n",
                    modth.gnth,modth.gnth > 1 ? "s" : "",
                    cr->nnodes > 1 ? mpi_str : "");
        }
        if (bSepPME && modth.gnth_pme != modth.gnth)
        {
            fprintf(stderr, "Using %d OpenMP thread%s %s for PME\n",
                    modth.gnth_pme,modth.gnth_pme > 1 ? "s" : "",
                    cr->nnodes > 1 ? mpi_str : "");
        }
    }

    /* detect and warn about oversubscription
     * TODO: enable this for separate PME nodes as well! */
    if (!bSepPME && cr->nodeid_intra == 0)
    {
        char sbuf[STRLEN], sbuf1[STRLEN], sbuf2[STRLEN];

        if (modth.gnth*nppn > nthreads_hw_avail)
        {
            sprintf(sbuf, "threads");
            sbuf1[0] = '\0';
            sprintf(sbuf2, "O");
#ifdef GMX_MPI
            if (modth.gnth == 1)
            {
#ifdef GMX_THREAD_MPI
                sprintf(sbuf, "thread-MPI threads");
#else
                sprintf(sbuf, "MPI processes");
                sprintf(sbuf1, " per node");
                sprintf(sbuf2, "On node %d: o", cr->sim_nodeid);
#endif
            }
#endif
            md_print_warn(cr, fplog,
                          "WARNING: %sversubscribing the available %d logical CPU cores%s with %d %s.\n"
                          "         This will cause considerable performance loss!",
                          sbuf2, nthreads_hw_avail, sbuf1, nppn*modth.gnth, sbuf);
        }
    }
}

int gmx_omp_nthreads_get(int mod)
{
    if (mod < 0 || mod >= emntNR)
    {
        /* invalid module queried */
        return -1;
    }
    else
    {
        return modth.nth[mod];
    }
}