/* Function to evaluate F-value in permutest.cca. For instance, in R
 * CMD check in Macbook Air this function uses 1/4 of computing time,
 * and in applications with constrained ordination this is the major
 * function. Even small speed-up in this function will have a
 * considerable impact in running time.
 */

#include <R.h>
#include <Rinternals.h>
#include <R_ext/Linpack.h> /* QR */
#include <R_ext/Lapack.h>  /* SVD */

/* LINPACK uses the same function (dqrsl) to find derived results from
 * the QR decomposition. It uses decimal coding to define the kind of
 * output with following alternatives (although we will not use all of
 * these): */

#define FIT 1
#define RESID 10
#define COEF 100
#define QTY 1000
#define QY 10000

/* Function called form getF to evaluate the sum of all eigenvalues */

static double getEV(double *x, int nr, int nc, int isDB)
{
    int i, ii;
    double sumev;
    switch(isDB) {
    case 1:
	for(i = 0, sumev = 0; i < nr; i++) {
	    ii = i * nr + i;
	    sumev += x[ii] * x[ii];
	}
	break;
    case 0:
	for(i = 0, sumev = 0; i < nr * nc; i++)
	    sumev += x[i] * x[i];
	break;
    }
    return sumev;
}

/* LAPACK function dgesdd for SVD. Returns first singular value */

static double svdfirst(double *x, int nr, int nc)
{
    char jobz[2] = "N";
    int minrc = (nr < nc) ? nr : nc;
    int i, len = nr*nc, info, lwork;
    double dummy = 0, query;

    /* copy data: dgesdd will destroy the original */
    double *xwork = (double *) R_alloc(len, sizeof(double));
    for(i = 0; i < len; i++)
	xwork[i] = x[i];
    /* singular values */
    double *sigma = (double *) R_alloc(minrc, sizeof(double));

    int *iwork = (int *) R_alloc(8 * minrc, sizeof(int));
    /* query and set optimal work array */
    info = 0;
    lwork = -1;
    F77_CALL(dgesdd)(jobz, &nr, &nc, xwork, &nr, sigma, &dummy,
		     &nr, &dummy, &nc, &query, &lwork, iwork, &info);
    if (info != 0)
	error("error %d from Lapack dgesdd", info);
    lwork = (int) query;
    double *work = (double *) R_alloc(lwork, sizeof(double));
    /* call svd */
    F77_CALL(dgesdd)(jobz, &nr, &nc, xwork, &nr, sigma, &dummy,
		     &nr, &dummy, &nc, work, &lwork, iwork, &info);
    if (info != 0)
	error("error %d from Lapack dgesdd, pos 2", info);
    return sigma[0];
}

/* function to test previous from R */
SEXP test_svd(SEXP x)
{
    int nr = nrows(x), nc = ncols(x);
    SEXP ans = PROTECT(allocVector(REALSXP, 1));
    REAL(ans)[0] = svdfirst(REAL(x), nr, nc);
    UNPROTECT(1);
    return ans;
}

/* Function do_getF is modelled after R function getF embedded in
 * permutest.cca. The do_getF provides a drop-in replacement to the R
 * function, and is called directly the R function */

SEXP do_getF(SEXP perms, SEXP E, SEXP QR, SEXP QZ, SEXP first,
	     SEXP isPartial)
{
    int i, j, k, ki,
	nperm = nrows(perms), nr = nrows(E), nc = ncols(E),
	FIRST = asInteger(first), PARTIAL = asInteger(isPartial);
    double ev1;
    SEXP ans = PROTECT(allocMatrix(REALSXP, nperm, 2));
    double *rans = REAL(ans);
    SEXP Y = PROTECT(duplicate(E));
    double *rY = REAL(Y);

    /* pointers and new objects to the QR decomposition */

    double *qr = REAL(VECTOR_ELT(QR, 0));
    int qrank = asInteger(VECTOR_ELT(QR, 1));
    double *qraux = REAL(VECTOR_ELT(QR, 2));
    double *Zqr, *Zqraux;
    int Zqrank;
    if (PARTIAL) {
	Zqr = REAL(VECTOR_ELT(QZ, 0));
	Zqrank = asInteger(VECTOR_ELT(QZ, 1));
	Zqraux = REAL(VECTOR_ELT(QZ, 2));
    }

    double *fitted = (double *) R_alloc(nr * nc, sizeof(double));
    double *resid = (double *) R_alloc(nr * nc, sizeof(double));
    double *qty = (double *) R_alloc(nr, sizeof(double));
    double dummy;
    int info, qrkind;

    /* double *wtake = (double *) R_alloc(nr, sizeof(double)); */

    /* permutation matrix must be duplicated */
    SEXP dperms = PROTECT(duplicate(perms));
    int *iperm = INTEGER(dperms);

    /* permutations to zero base */
    for(i = 0; i < nperm * nr; i++)
	iperm[i]--;

    /* loop over rows of permutation matrix */
    for (k = 0; k < nperm; k++) {
	/* Y will be permuted data */
	for (i = 0; i < nr; i++) {
	    ki = iperm[k + nperm * i];
	    for(j = 0; j < nc; j++) {
		rY[i + nr * j] = REAL(E)[ki + nr * j];
	    }
	}

	/* Partial model: qr.resid(QZ, Y) with LINPACK */
	if (PARTIAL) {
	    qrkind = RESID;
	    for(i = 0; i < nc; i++)
		F77_CALL(dqrsl)(Zqr, &nr, &nr, &Zqrank, Zqraux, rY + i*nr,
				&dummy, qty, &dummy, rY + i*nr, &dummy,
				&qrkind, &info);
	}

	/* qr.fitted(QR, Y) + qr.resid(QR, Y) with LINPACK */
	if (PARTIAL || FIRST)
	    qrkind = FIT + RESID;
	else
	    qrkind = FIT;
	for (i = 0; i < nc; i++)
	    F77_CALL(dqrsl)(qr, &nr, &nr, &qrank, qraux, rY + i*nr, &dummy,
			    qty, &dummy, resid + i*nr, fitted + i*nr,
			    &qrkind, &info);

	/* Eigenvalues: either sum of all or the first If the sum of
	 * all eigenvalues does not change, we have only ev of CCA
	 * component in the first column, and the second column is
	 * rubbish that should be filled in the calling R function
	 * with the correct value. */

	if (FIRST) {
	    ev1 = svdfirst(fitted, nr, nc);
	    rans[k] = ev1 * ev1;
	} else {
	    rans[k] = getEV(fitted, nr, nc, 0);
	}
	if (PARTIAL || FIRST)
	    rans[k + nperm] = getEV(resid, nr, nc, 0);

    } /* end permutation loop */

    UNPROTECT(3);
    return ans;
}