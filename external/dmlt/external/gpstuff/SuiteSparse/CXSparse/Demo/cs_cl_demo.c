#include "cs_cl_demo.h"
#include <time.h>
/* 1 if A is square & upper tri., -1 if square & lower tri., 0 otherwise */
static UF_long is_sym (cs_cl *A)
{
    UF_long is_upper, is_lower, j, p, n = A->n, m = A->m, *Ap = A->p, *Ai = A->i ;
    if (m != n) return (0) ;
    is_upper = 1 ;
    is_lower = 1 ;
    for (j = 0 ; j < n ; j++)
    {
        for (p = Ap [j] ; p < Ap [j+1] ; p++)
        {
            if (Ai [p] > j) is_upper = 0 ;
            if (Ai [p] < j) is_lower = 0 ;
        }
    }
    return (is_upper ? 1 : (is_lower ? -1 : 0)) ;
}

/* true for off-diagonal entries */
static UF_long dropdiag (UF_long i, UF_long j, cs_complex_t aij, void *other) { return (i != j) ;}

/* C = A + triu(A,1)' */
static cs_cl *make_sym (cs_cl *A)
{
    cs_cl *AT, *C ;
    AT = cs_cl_transpose (A, 1) ;          /* AT = A' */
    cs_cl_fkeep (AT, &dropdiag, NULL) ;    /* drop diagonal entries from AT */
    C = cs_cl_add (A, AT, 1, 1) ;          /* C = A+AT */
    cs_cl_spfree (AT) ;
    return (C) ;
}

/* create a right-hand side */
static void rhs (cs_complex_t *x, cs_complex_t *b, UF_long m)
{
    UF_long i ;
    for (i = 0 ; i < m ; i++) b [i] = 1 + ((double) i) / m ;
    for (i = 0 ; i < m ; i++) x [i] = b [i] ;
}

/* infinity-norm of x */
static double norm (cs_complex_t *x, UF_long n)
{
    UF_long i ;
    double normx = 0 ;
    for (i = 0 ; i < n ; i++) normx = CS_MAX (normx, cabs (x [i])) ;
    return (normx) ;
}

/* compute residual, norm(A*x-b,inf) / (norm(A,1)*norm(x,inf) + norm(b,inf)) */
static void print_resid (UF_long ok, cs_cl *A, cs_complex_t *x, cs_complex_t *b, cs_complex_t *resid)
{
    UF_long i, m, n ;
    if (!ok) { printf ("    (failed)\n") ; return ; }
    m = A->m ; n = A->n ;
    for (i = 0 ; i < m ; i++) resid [i] = -b [i] ;  /* resid = -b */
    cs_cl_gaxpy (A, x, resid) ;                        /* resid = resid + A*x  */
    printf ("resid: %8.2e\n", norm (resid,m) / ((n == 0) ? 1 :
        (cs_cl_norm (A) * norm (x,n) + norm (b,m)))) ;
}

static double tic (void) { return (clock () / (double) CLOCKS_PER_SEC) ; }
static double toc (double t) { double s = tic () ; return (CS_MAX (0, s-t)) ; }

static void print_order (UF_long order)
{
    switch (order)
    {
        case 0: printf ("natural    ") ; break ;
        case 1: printf ("amd(A+A')  ") ; break ;
        case 2: printf ("amd(S'*S)  ") ; break ;
        case 3: printf ("amd(A'*A)  ") ; break ;
    }
}

/* read a problem from a file */
problem *get_problem (FILE *f, double tol)
{
    cs_cl *T, *A, *C ;
    UF_long sym, m, n, mn, nz1, nz2 ;
    problem *Prob ;
    Prob = cs_cl_calloc (1, sizeof (problem)) ;
    if (!Prob) return (NULL) ;
    T = cs_cl_load (f) ;                   /* load triplet matrix T from a file */
    Prob->A = A = cs_cl_compress (T) ;     /* A = compressed-column form of T */
    cs_cl_spfree (T) ;                     /* clear T */
    if (!cs_cl_dupl (A)) return (free_problem (Prob)) ; /* sum up duplicates */
    Prob->sym = sym = is_sym (A) ;      /* determine if A is symmetric */
    m = A->m ; n = A->n ;
    mn = CS_MAX (m,n) ;
    nz1 = A->p [n] ;
    cs_cl_dropzeros (A) ;                  /* drop zero entries */
    nz2 = A->p [n] ;
    if (tol > 0) cs_cl_droptol (A, tol) ;  /* drop tiny entries (just to test) */
    Prob->C = C = sym ? make_sym (A) : A ;  /* C = A + triu(A,1)', or C=A */
    if (!C) return (free_problem (Prob)) ;
    printf ("\n--- Matrix: %ld-by-%ld, nnz: %ld (sym: %ld: nnz %ld), norm: %8.2e\n",
            m, n, A->p [n], sym, sym ? C->p [n] : 0, cs_cl_norm (C)) ;
    if (nz1 != nz2) printf ("zero entries dropped: %ld\n", nz1 - nz2) ;
    if (nz2 != A->p [n]) printf ("tiny entries dropped: %ld\n", nz2 - A->p [n]) ;
    Prob->b = cs_cl_malloc (mn, sizeof (cs_complex_t)) ;
    Prob->x = cs_cl_malloc (mn, sizeof (cs_complex_t)) ;
    Prob->resid = cs_cl_malloc (mn, sizeof (cs_complex_t)) ;
    return ((!Prob->b || !Prob->x || !Prob->resid) ? free_problem (Prob) : Prob) ;
}

/* free a problem */
problem *free_problem (problem *Prob)
{
    if (!Prob) return (NULL) ;
    cs_cl_spfree (Prob->A) ;
    if (Prob->sym) cs_cl_spfree (Prob->C) ;
    cs_cl_free (Prob->b) ;
    cs_cl_free (Prob->x) ;
    cs_cl_free (Prob->resid) ;
    return (cs_cl_free (Prob)) ;
}

/* solve a linear system using Cholesky, LU, and QR, with various orderings */
UF_long demo2 (problem *Prob)
{
    cs_cl *A, *C ;
    cs_complex_t *b, *x, *resid ;
    double t, tol ;
    UF_long k, m, n, ok, order, nb, ns, *r, *s, *rr, sprank ;
    cs_cld *D ;
    if (!Prob) return (0) ;
    A = Prob->A ; C = Prob->C ; b = Prob->b ; x = Prob->x ; resid = Prob->resid;
    m = A->m ; n = A->n ;
    tol = Prob->sym ? 0.001 : 1 ;               /* partial pivoting tolerance */
    D = cs_cl_dmperm (C, 1) ;                      /* randomized dmperm analysis */
    if (!D) return (0) ;
    nb = D->nb ; r = D->r ; s = D->s ; rr = D->rr ;
    sprank = rr [3] ;
    for (ns = 0, k = 0 ; k < nb ; k++)
    {
        ns += ((r [k+1] == r [k]+1) && (s [k+1] == s [k]+1)) ;
    }
    printf ("blocks: %ld singletons: %ld structural rank: %ld\n", nb, ns, sprank) ;
    cs_cl_dfree (D) ;
    for (order = 0 ; order <= 3 ; order += 3)   /* natural and amd(A'*A) */
    {
        if (!order && m > 1000) continue ;
        printf ("QR   ") ;
        print_order (order) ;
        rhs (x, b, m) ;                         /* compute right-hand side */
        t = tic () ;
        ok = cs_cl_qrsol (order, C, x) ;           /* min norm(Ax-b) with QR */
        printf ("time: %8.2f ", toc (t)) ;
        print_resid (ok, C, x, b, resid) ;      /* print residual */
    }
    if (m != n || sprank < n) return (1) ;      /* return if rect. or singular*/
    for (order = 0 ; order <= 3 ; order++)      /* try all orderings */
    {
        if (!order && m > 1000) continue ;
        printf ("LU   ") ;
        print_order (order) ;
        rhs (x, b, m) ;                         /* compute right-hand side */
        t = tic () ;
        ok = cs_cl_lusol (order, C, x, tol) ;      /* solve Ax=b with LU */
        printf ("time: %8.2f ", toc (t)) ;
        print_resid (ok, C, x, b, resid) ;      /* print residual */
    }
    if (!Prob->sym) return (1) ;
    for (order = 0 ; order <= 1 ; order++)      /* natural and amd(A+A') */
    {
        if (!order && m > 1000) continue ;
        printf ("Chol ") ;
        print_order (order) ;
        rhs (x, b, m) ;                         /* compute right-hand side */
        t = tic () ;
        ok = cs_cl_cholsol (order, C, x) ;         /* solve Ax=b with Cholesky */
        printf ("time: %8.2f ", toc (t)) ;
        print_resid (ok, C, x, b, resid) ;      /* print residual */
    }
    return (1) ;
} 

/* free workspace for demo3 */
static UF_long done3 (UF_long ok, cs_cls *S, cs_cln *N, cs_complex_t *y, cs_cl *W, cs_cl *E, UF_long *p)
{
    cs_cl_sfree (S) ;
    cs_cl_nfree (N) ;
    cs_cl_free (y) ;
    cs_cl_spfree (W) ;
    cs_cl_spfree (E) ;
    cs_cl_free (p) ;
    return (ok) ;
}

/* Cholesky update/downdate */
UF_long demo3 (problem *Prob)
{
    cs_cl *A, *C, *W = NULL, *WW, *WT, *E = NULL, *W2 ;
    UF_long n, k, *Li, *Lp, *Wi, *Wp, p1, p2, *p = NULL, ok ;
    cs_complex_t *b, *x, *resid, *y = NULL, *Lx, *Wx, s ;
    double t, t1 ;
    cs_cls *S = NULL ;
    cs_cln *N = NULL ;
    if (!Prob || !Prob->sym || Prob->A->n == 0) return (0) ;
    A = Prob->A ; C = Prob->C ; b = Prob->b ; x = Prob->x ; resid = Prob->resid;
    n = A->n ;
    if (!Prob->sym || n == 0) return (1) ;
    rhs (x, b, n) ;                             /* compute right-hand side */
    printf ("\nchol then update/downdate ") ;
    print_order (1) ;
    y = cs_cl_malloc (n, sizeof (cs_complex_t)) ;
    t = tic () ;
    S = cs_cl_schol (1, C) ;                       /* symbolic Chol, amd(A+A') */
    printf ("\nsymbolic chol time %8.2f\n", toc (t)) ;
    t = tic () ;
    N = cs_cl_chol (C, S) ;                        /* numeric Cholesky */
    printf ("numeric  chol time %8.2f\n", toc (t)) ;
    if (!S || !N || !y) return (done3 (0, S, N, y, W, E, p)) ;
    t = tic () ;
    cs_cl_ipvec (S->pinv, b, y, n) ;               /* y = P*b */
    cs_cl_lsolve (N->L, y) ;                       /* y = L\y */
    cs_cl_ltsolve (N->L, y) ;                      /* y = L'\y */
    cs_cl_pvec (S->pinv, y, x, n) ;                /* x = P'*y */
    printf ("solve    chol time %8.2f\n", toc (t)) ;
    printf ("original: ") ;
    print_resid (1, C, x, b, resid) ;           /* print residual */
    k = n/2 ;                                   /* construct W  */
    W = cs_cl_spalloc (n, 1, n, 1, 0) ;
    if (!W) return (done3 (0, S, N, y, W, E, p)) ;
    Lp = N->L->p ; Li = N->L->i ; Lx = N->L->x ;
    Wp = W->p ; Wi = W->i ; Wx = W->x ;
    Wp [0] = 0 ;
    p1 = Lp [k] ;
    Wp [1] = Lp [k+1] - p1 ;
    s = Lx [p1] ;
    srand (1) ;
    for ( ; p1 < Lp [k+1] ; p1++)
    {
        p2 = p1 - Lp [k] ;
        Wi [p2] = Li [p1] ;
        Wx [p2] = s * rand () / ((double) RAND_MAX) ;
    }
    t = tic () ;
    ok = cs_cl_updown (N->L, +1, W, S->parent) ;   /* update: L*L'+W*W' */
    t1 = toc (t) ;
    printf ("update:   time: %8.2f\n", t1) ;
    if (!ok) return (done3 (0, S, N, y, W, E, p)) ;
    t = tic () ;
    cs_cl_ipvec (S->pinv, b, y, n) ;               /* y = P*b */
    cs_cl_lsolve (N->L, y) ;                       /* y = L\y */
    cs_cl_ltsolve (N->L, y) ;                      /* y = L'\y */
    cs_cl_pvec (S->pinv, y, x, n) ;                /* x = P'*y */
    t = toc (t) ;
    p = cs_cl_pinv (S->pinv, n) ;
    W2 = cs_cl_permute (W, p, NULL, 1) ;           /* E = C + (P'W)*(P'W)' */
    WT = cs_cl_transpose (W2,1) ;
    WW = cs_cl_multiply (W2, WT) ;
    cs_cl_spfree (WT) ;
    cs_cl_spfree (W2) ;
    E = cs_cl_add (C, WW, 1, 1) ;
    cs_cl_spfree (WW) ;
    if (!E || !p) return (done3 (0, S, N, y, W, E, p)) ;
    printf ("update:   time: %8.2f (incl solve) ", t1+t) ;
    print_resid (1, E, x, b, resid) ;           /* print residual */
    cs_cl_nfree (N) ;                              /* clear N */
    t = tic () ;
    N = cs_cl_chol (E, S) ;                        /* numeric Cholesky */
    if (!N) return (done3 (0, S, N, y, W, E, p)) ;
    cs_cl_ipvec (S->pinv, b, y, n) ;               /* y = P*b */
    cs_cl_lsolve (N->L, y) ;                       /* y = L\y */
    cs_cl_ltsolve (N->L, y) ;                      /* y = L'\y */
    cs_cl_pvec (S->pinv, y, x, n) ;                /* x = P'*y */
    t = toc (t) ;
    printf ("rechol:   time: %8.2f (incl solve) ", t) ;
    print_resid (1, E, x, b, resid) ;           /* print residual */
    t = tic () ;
    ok = cs_cl_updown (N->L, -1, W, S->parent) ;   /* downdate: L*L'-W*W' */
    t1 = toc (t) ;
    if (!ok) return (done3 (0, S, N, y, W, E, p)) ;
    printf ("downdate: time: %8.2f\n", t1) ;
    t = tic () ;
    cs_cl_ipvec (S->pinv, b, y, n) ;               /* y = P*b */
    cs_cl_lsolve (N->L, y) ;                       /* y = L\y */
    cs_cl_ltsolve (N->L, y) ;                      /* y = L'\y */
    cs_cl_pvec (S->pinv, y, x, n) ;                /* x = P'*y */
    t = toc (t) ;
    printf ("downdate: time: %8.2f (incl solve) ", t1+t) ;
    print_resid (1, C, x, b, resid) ;           /* print residual */
    return (done3 (1, S, N, y, W, E, p)) ;
} 