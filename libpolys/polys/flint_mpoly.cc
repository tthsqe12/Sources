// emacs edit mode for this file is -*- C++ -*-
/****************************************
*  Computer Algebra System SINGULAR     *
****************************************/
/*
* ABSTRACT: flint mpoly
*/

#include "misc/auxiliary.h"
#include "flintconv.h"
#include "flint_mpoly.h"
#include <vector>

#ifdef HAVE_FLINT
#if __FLINT_RELEASE >= 20503
#include "coeffs/coeffs.h"
#include "coeffs/longrat.h"
#include "polys/monomials/p_polys.h"

/*
system("--threads", 5);
ring r = 0, (z,y,x), lp;
poly p = (1+x+3*y+z/2)^3;
p*p;


ring r = 101, (z,y,x), lp;
poly p = (1+x+3*y+z/2)^3;
p*p;

*/

/* fmpq_mpoly ****************************************************************/


BOOLEAN convSingRFlintR(fmpq_mpoly_ctx_t ctx, const ring r)
{
    if (rRing_ord_pure_dp(r))
    {
        fmpq_mpoly_ctx_init(ctx,r->N,ORD_DEGREVLEX);
        return FALSE;
    }
    else if (rRing_ord_pure_Dp(r))
    {
        fmpq_mpoly_ctx_init(ctx,r->N,ORD_DEGLEX);
        return FALSE;
    }
    else if (rRing_ord_pure_lp(r))
    {
        fmpq_mpoly_ctx_init(ctx,r->N,ORD_LEX);
        return FALSE;
    }
    return TRUE;
}

void my_convSingNFlintN_QQ(fmpq_t f, number n)
{
    if (SR_HDL(n)&SR_INT)
    {
        fmpq_set_si(f,SR_TO_INT(n),1);
    }
    else if (n->s<3)
    {
        fmpz_set_mpz(fmpq_numref(f), n->z);
        fmpz_set_mpz(fmpq_denref(f), n->n);
    }
    else
    {
        fmpz_set_mpz(fmpq_numref(f), n->z);
        fmpz_one(fmpq_denref(f));
    }
}



class convert_sing_to_fmpq_mpoly_base
{
public:
    slong num_threads;
    slong length;
    fmpq_mpoly_struct * res;
    const fmpq_mpoly_ctx_struct * ctx;
    std::vector<poly> markers;
    ring r;

    convert_sing_to_fmpq_mpoly_base(
        slong num_threads_,
        fmpq_mpoly_struct * res_,
        const fmpq_mpoly_ctx_struct * ctx_,
        const ring r_)
      : num_threads(num_threads_),
        res(res_),
        ctx(ctx_),
        r(r_)
    {
    }

    void init_poly(poly p)
    {
        length = 0;
        while (p != NULL)
        {
            markers.push_back(p);
            for (int i = 4096; i > 0; i--)
            {
                pIter(p);
                length++;
                if (p == NULL)
                    break;
            }
        }

        fmpq_mpoly_init3(res, length, SI_LOG2(r->bitmask), ctx);
    }
};

class convert_sing_to_fmpq_mpoly_arg
{
public:
    fmpq_t content;
    slong start_idx, end_idx;
    convert_sing_to_fmpq_mpoly_base* base;

    convert_sing_to_fmpq_mpoly_arg() {fmpq_init(content);}
    ~convert_sing_to_fmpq_mpoly_arg() {fmpq_clear(content);}
};

void convert_sing_to_fmpq_mpoly_content_worker(void * varg)
{
    convert_sing_to_fmpq_mpoly_arg* arg = (convert_sing_to_fmpq_mpoly_arg*) varg;
    fmpq_t c;
    fmpq_init(c);

    slong idx = arg->start_idx/4096;
    poly p = arg->base->markers[idx];
    idx *= 4096;
    while (idx < arg->start_idx)
    {
        pIter(p);
        idx++;
    }

    /* first find gcd of coeffs */
    fmpq_zero(arg->content);
    while (idx < arg->end_idx)
    {
        my_convSingNFlintN_QQ(c, number(pGetCoeff(p)));
        fmpq_gcd(arg->content, arg->content, c);
        pIter(p);
        idx++;
    }

    fmpq_clear(c);
}

void convert_sing_to_fmpq_mpoly_zpoly_worker(void * varg)
{
    convert_sing_to_fmpq_mpoly_arg * arg = (convert_sing_to_fmpq_mpoly_arg *) varg;
    convert_sing_to_fmpq_mpoly_base * base = arg->base;
    ulong * exp = (ulong*) flint_malloc((base->r->N+1)*sizeof(ulong));
    fmpq_t c, t;
    fmpq_init(c);
    fmpq_init(t);

    slong idx = arg->start_idx/4096;
    poly p = base->markers[idx];
    idx *= 4096;
    while (idx < arg->start_idx)
    {
        pIter(p);
        idx++;
    }

    while (idx < arg->end_idx)
    {
        my_convSingNFlintN_QQ(c, number(pGetCoeff(p)));
        FLINT_ASSERT(!fmpq_is_zero(base->res->content));
        fmpq_div(t, c, base->res->content);
        FLINT_ASSERT(fmpz_is_one(fmpq_denref(t)));

        slong N = mpoly_words_per_exp(base->res->zpoly->bits, base->ctx->zctx->minfo);
        fmpz_swap(base->res->zpoly->coeffs + idx, fmpq_numref(t));
        #if SIZEOF_LONG==8
        p_GetExpVL(p, (int64*)exp, base->r);
        mpoly_set_monomial_ui(base->res->zpoly->exps + N*idx, exp, base->res->zpoly->bits, base->ctx->zctx->minfo);
        #else
        p_GetExpV(p, (int*)exp, base->r);
        mpoly_set_monomial_ui(base->res->zpoly->exps + N*idx, &(exp[1]), base->res->zpoly->bits, base->ctx->minfo);
        #endif

        pIter(p);
        idx++;
    }

    flint_free(exp);
    fmpq_clear(c);
    fmpq_clear(t);
}

/* res comes in unitialized!!!! */
void convSingPFlintMP(
    fmpq_mpoly_t res,
    fmpq_mpoly_ctx_t ctx,
    poly p,
    const ring r,
    slong thread_limit)
{
    thread_pool_handle * handles;
    slong num_handles;

    /* get workers */
    handles = NULL;
    num_handles = 0;
    if (thread_limit > 1 && global_thread_pool_initialized)
    {
        slong max_num_handles = thread_pool_get_size(global_thread_pool);
        max_num_handles = FLINT_MIN(thread_limit - 1, max_num_handles);
        if (max_num_handles > 0)
        {
            handles = (thread_pool_handle *) flint_malloc(max_num_handles*sizeof(thread_pool_handle));
            num_handles = thread_pool_request(global_thread_pool, handles, max_num_handles);
        }
    }

    convert_sing_to_fmpq_mpoly_base base(num_handles + 1, res, ctx, r);
    base.init_poly(p);

    convert_sing_to_fmpq_mpoly_arg * args = new convert_sing_to_fmpq_mpoly_arg[base.num_threads];
    for (slong i = 0; i < base.num_threads; i++)
    {
        args[i].base = &base;
        args[i].start_idx = (i+0)*base.length/base.num_threads;
        args[i].end_idx   = (i+1)*base.length/base.num_threads;
    }

    /* get content */
    for (slong i = 0; i < num_handles; i++)
        thread_pool_wake(global_thread_pool, handles[i], convert_sing_to_fmpq_mpoly_content_worker, args + i);
    convert_sing_to_fmpq_mpoly_content_worker(args + num_handles);
    for (slong i = 0; i < num_handles; i++)
    {
        thread_pool_wait(global_thread_pool, handles[i]);
printf("sing -> flint wait done %d\n", i);
    }

    /* sign of content should match sign of first coeff */
    fmpq_zero(base.res->content);
    for (slong i = 0; i < base.num_threads; i++)
        fmpq_gcd(base.res->content, base.res->content, args[i].content);
    if (p != NULL)
    {
        fmpq_t c;
        fmpq_init(c);
        my_convSingNFlintN_QQ(c, number(pGetCoeff(p)));
        if (fmpq_sgn(c) < 0)
            fmpq_neg(base.res->content, base.res->content);
        fmpq_clear(c);
    }

    /* fill in res->zpoly */
    for (slong i = 0; i < num_handles; i++)
        thread_pool_wake(global_thread_pool, handles[i], convert_sing_to_fmpq_mpoly_zpoly_worker, args + i);
    convert_sing_to_fmpq_mpoly_zpoly_worker(args + num_handles);
    for (slong i = 0; i < num_handles; i++)
        thread_pool_wait(global_thread_pool, handles[i]);

    base.res->zpoly->length = base.length;

    delete[] args;

    for (slong i = 0; i < num_handles; i++)
        thread_pool_give_back(global_thread_pool, handles[i]);
    if (handles)
        flint_free(handles);

    if (!fmpq_mpoly_is_canonical(res, ctx))
    {
        printf("bad res: "); fmpq_mpoly_print_pretty(res, (const char**) r->names,ctx);PrintLn();
        FLINT_ASSERT(0);
    }

printf("res: "); fmpq_mpoly_print_pretty(res, (const char**) r->names,ctx);PrintLn();
}



class convert_fmpq_mpoly_to_sing_base
{
public:
    slong num_threads;
    fmpq_mpoly_struct * f;
    const fmpq_mpoly_ctx_struct * ctx;
    ring r;

    convert_fmpq_mpoly_to_sing_base(
        slong num_threads_,
        fmpq_mpoly_struct * f_,
        const fmpq_mpoly_ctx_struct * ctx_,
        const ring r_)
      : num_threads(num_threads_),
        f(f_),
        ctx(ctx_),
        r(r_)
    {
    }
};

class convert_fmpq_mpoly_to_sing_arg
{
public:
    poly poly_start, poly_end;
    slong start_idx, end_idx;
    convert_fmpq_mpoly_to_sing_base* base;
};

void convert_fmpq_mpoly_to_sing_worker(void * varg)
{
    convert_fmpq_mpoly_to_sing_arg * arg = (convert_fmpq_mpoly_to_sing_arg *) varg;
    convert_fmpq_mpoly_to_sing_base * base = arg->base;
    ulong* exp = (ulong*) flint_malloc((base->r->N+1)*sizeof(ulong));
    fmpq_t c;
    fmpq_init(c);

    for (slong idx = arg->end_idx - 1; idx >= arg->start_idx; idx--)
    {
        poly pp = p_Init(base->r);

        #if SIZEOF_LONG==8
        fmpq_mpoly_get_term_exp_ui(exp, base->f, idx, base->ctx);
        p_SetExpVL(pp, (int64*)exp, base->r);
        #else
        fmpq_mpoly_get_term_exp_ui(&(exp[1]), base->f, idx, base->ctx);
        p_SetExpV(pp, (int*)exp, base->r);
        #endif
        p_Setm(pp, base->r);

        fmpq_mpoly_get_term_coeff_fmpq(c, base->f, idx, base->ctx);
        pSetCoeff0(pp, number(convFlintNSingN_QQ(c, base->r->cf)));

        if (idx == arg->end_idx - 1)
            arg->poly_end = pp;

        pNext(pp) = arg->poly_start;
        arg->poly_start = pp;
    }

    flint_free(exp);
    fmpq_clear(c);
}


poly convFlintMPSingP(
    fmpq_mpoly_t f,
    fmpq_mpoly_ctx_t ctx,
    const ring r,
    slong thread_limit)
{
    thread_pool_handle * handles;
    slong num_handles;

    /* get workers */
    handles = NULL;
    num_handles = 0;
    if (thread_limit > 1 && global_thread_pool_initialized)
    {
        slong max_num_handles = thread_pool_get_size(global_thread_pool);
        max_num_handles = FLINT_MIN(thread_limit - 1, max_num_handles);
        if (max_num_handles > 0)
        {
            handles = (thread_pool_handle *) flint_malloc(max_num_handles*sizeof(thread_pool_handle));
            num_handles = thread_pool_request(global_thread_pool, handles, max_num_handles);
        }
    }

    convert_fmpq_mpoly_to_sing_base base(num_handles + 1, f, ctx, r);

    convert_fmpq_mpoly_to_sing_arg * args = new convert_fmpq_mpoly_to_sing_arg[base.num_threads];
    for (slong i = 0; i < base.num_threads; i++)
    {
        args[i].base = &base;
        args[i].start_idx = (i+0)*base.f->zpoly->length/base.num_threads;
        args[i].end_idx   = (i+1)*base.f->zpoly->length/base.num_threads;
        args[i].poly_end = NULL;
        args[i].poly_start = NULL;
    }

    /* construct pieces */
printf("constructing pieces\n");

    for (slong i = 0; i < num_handles; i++)
        thread_pool_wake(global_thread_pool, handles[i], convert_fmpq_mpoly_to_sing_worker, args + i);
    convert_fmpq_mpoly_to_sing_worker(args + num_handles);
    for (slong i = 0; i < num_handles; i++)
    {
        thread_pool_wait(global_thread_pool, handles[i]);
printf("flint -> sing wait done %d\n", i);
    }

printf("joining\n");

    /* join pieces */
    poly p = NULL;
    for (slong i = num_handles; i >= 0; i--)
    {
        if (args[i].poly_end != NULL)
        {
            pNext(args[i].poly_end) = p;
            p = args[i].poly_start;
        }
    }

    for (slong i = 0; i < num_handles; i++)
        thread_pool_give_back(global_thread_pool, handles[i]);
    if (handles)
        flint_free(handles);

    p_Test(p,r);
    return p;
}

poly Flint_Mult_MP(poly p, poly q, fmpq_mpoly_ctx_t ctx, const ring r)
{
    fmpq_mpoly_t pp, qq, res;
    convSingPFlintMP(pp, ctx, p, r, MPOLY_DEFAULT_THREAD_LIMIT);
    convSingPFlintMP(qq, ctx, q, r, MPOLY_DEFAULT_THREAD_LIMIT);
    fmpq_mpoly_init(res, ctx);
    fmpq_mpoly_mul(res, pp, qq, ctx);
    poly pres = convFlintMPSingP(res, ctx, r, MPOLY_DEFAULT_THREAD_LIMIT);
    fmpq_mpoly_clear(res, ctx);
    fmpq_mpoly_clear(pp, ctx);
    fmpq_mpoly_clear(qq, ctx);
    fmpq_mpoly_ctx_clear(ctx);
    p_Test(pres, r);
    return pres;
}


/* nmod_mpoly ****************************************************************/

BOOLEAN convSingRFlintR(nmod_mpoly_ctx_t ctx, const ring r)
{
    if (rRing_ord_pure_dp(r))
    {
        nmod_mpoly_ctx_init(ctx,r->N,ORD_DEGREVLEX,r->cf->ch);
        return FALSE;
    }
    else if (rRing_ord_pure_Dp(r))
    {
        nmod_mpoly_ctx_init(ctx,r->N,ORD_DEGLEX,r->cf->ch);
        return FALSE;
    }
    else if (rRing_ord_pure_lp(r))
    {
        nmod_mpoly_ctx_init(ctx,r->N,ORD_LEX,r->cf->ch);
        return FALSE;
    }
    return TRUE;
}


class convert_sing_to_nmod_mpoly_base
{
public:
    slong num_threads;
    slong length;
    nmod_mpoly_struct * res;
    const nmod_mpoly_ctx_struct * ctx;
    std::vector<poly> markers;
    ring r;

    convert_sing_to_nmod_mpoly_base(
        slong num_threads_,
        nmod_mpoly_struct * res_,
        const nmod_mpoly_ctx_struct * ctx_,
        const ring r_)
      : num_threads(num_threads_),
        res(res_),
        ctx(ctx_),
        r(r_)
    {
    }

    void init_poly(poly p)
    {
        length = 0;
        while (p != NULL)
        {
            markers.push_back(p);
            for (int i = 4096; i > 0; i--)
            {
                pIter(p);
                length++;
                if (p == NULL)
                    break;
            }
        }

        nmod_mpoly_init3(res, length, SI_LOG2(r->bitmask), ctx);
    }
};

class convert_sing_to_nmod_mpoly_arg
{
public:
    slong start_idx, end_idx;
    convert_sing_to_nmod_mpoly_base* base;
};


void convert_sing_to_nmod_mpoly_worker(void * varg)
{
    convert_sing_to_nmod_mpoly_arg * arg = (convert_sing_to_nmod_mpoly_arg *) varg;
    convert_sing_to_nmod_mpoly_base * base = arg->base;
    ulong * exp = (ulong*) flint_malloc((base->r->N+1)*sizeof(ulong));

    slong idx = arg->start_idx/4096;
    poly p = base->markers[idx];
    idx *= 4096;
    while (idx < arg->start_idx)
    {
        pIter(p);
        idx++;
    }

    while (idx < arg->end_idx)
    {
        slong N = mpoly_words_per_exp(base->res->bits, base->ctx->minfo);
        #if SIZEOF_LONG==8
        p_GetExpVL(p, (int64*)exp, base->r);
        mpoly_set_monomial_ui(base->res->exps + N*idx, exp, base->res->bits, base->ctx->minfo);
        #else
        p_GetExpV(p, (int*)exp, base->r);
        mpoly_set_monomial_ui(base->res->exps + N*idx, &(exp[1]), base->res->bits, base->ctx->minfo);
        #endif

        base->res->coeffs[idx] = (ulong)(number(pGetCoeff(p)));

        pIter(p);
        idx++;
    }

    flint_free(exp);
}

/* res comes in unitialized!!!! */
void convSingPFlintMP(
    nmod_mpoly_t res,
    nmod_mpoly_ctx_t ctx,
    poly p,
    const ring r,
    slong thread_limit)
{
    thread_pool_handle * handles;
    slong num_handles;

    /* get workers */
    handles = NULL;
    num_handles = 0;
    if (thread_limit > 1 && global_thread_pool_initialized)
    {
        slong max_num_handles = thread_pool_get_size(global_thread_pool);
        max_num_handles = FLINT_MIN(thread_limit - 1, max_num_handles);
        if (max_num_handles > 0)
        {
            handles = (thread_pool_handle *) flint_malloc(max_num_handles*sizeof(thread_pool_handle));
            num_handles = thread_pool_request(global_thread_pool, handles, max_num_handles);
        }
    }

    convert_sing_to_nmod_mpoly_base base(num_handles + 1, res, ctx, r);
    base.init_poly(p);

    convert_sing_to_nmod_mpoly_arg * args = new convert_sing_to_nmod_mpoly_arg[base.num_threads];
    for (slong i = 0; i < base.num_threads; i++)
    {
        args[i].base = &base;
        args[i].start_idx = (i+0)*base.length/base.num_threads;
        args[i].end_idx   = (i+1)*base.length/base.num_threads;
    }

    /* fill in res */
    for (slong i = 0; i < num_handles; i++)
        thread_pool_wake(global_thread_pool, handles[i], convert_sing_to_nmod_mpoly_worker, args + i);
    convert_sing_to_nmod_mpoly_worker(args + num_handles);
    for (slong i = 0; i < num_handles; i++)
        thread_pool_wait(global_thread_pool, handles[i]);

    base.res->length = base.length;

    delete[] args;

    for (slong i = 0; i < num_handles; i++)
        thread_pool_give_back(global_thread_pool, handles[i]);
    if (handles)
        flint_free(handles);

    if (!nmod_mpoly_is_canonical(res, ctx))
    {
        printf("bad res: "); nmod_mpoly_print_pretty(res, (const char**) r->names,ctx);PrintLn();
        FLINT_ASSERT(0);
    }

printf("res: "); nmod_mpoly_print_pretty(res, (const char**) r->names,ctx);PrintLn();
}




class convert_nmod_mpoly_to_sing_base
{
public:
    slong num_threads;
    nmod_mpoly_struct * f;
    const nmod_mpoly_ctx_struct * ctx;
    ring r;

    convert_nmod_mpoly_to_sing_base(
        slong num_threads_,
        nmod_mpoly_struct * f_,
        const nmod_mpoly_ctx_struct * ctx_,
        const ring r_)
      : num_threads(num_threads_),
        f(f_),
        ctx(ctx_),
        r(r_)
    {
    }
};

class convert_nmod_mpoly_to_sing_arg
{
public:
    poly poly_start, poly_end;
    slong start_idx, end_idx;
    convert_nmod_mpoly_to_sing_base* base;
};

void convert_nmod_mpoly_to_sing_worker(void * varg)
{
    convert_nmod_mpoly_to_sing_arg * arg = (convert_nmod_mpoly_to_sing_arg *) varg;
    convert_nmod_mpoly_to_sing_base * base = arg->base;
    ulong* exp = (ulong*) flint_malloc((base->r->N+1)*sizeof(ulong));

    for (slong idx = arg->end_idx - 1; idx >= arg->start_idx; idx--)
    {
        poly pp = p_Init(base->r);

        #if SIZEOF_LONG==8
        nmod_mpoly_get_term_exp_ui(exp, base->f, idx, base->ctx);
        p_SetExpVL(pp, (int64*)exp, base->r);
        #else
        nmod_mpoly_get_term_exp_ui(&(exp[1]), base->f, idx, base->ctx);
        p_SetExpV(pp, (int*)exp, base->r);
        #endif
        p_Setm(pp, base->r);

        pSetCoeff0(pp, number(nmod_mpoly_get_term_coeff_ui(base->f, idx, base->ctx)));

        if (idx == arg->end_idx - 1)
            arg->poly_end = pp;

        pNext(pp) = arg->poly_start;
        arg->poly_start = pp;
    }

    flint_free(exp);
}


poly convFlintMPSingP(
    nmod_mpoly_t f,
    nmod_mpoly_ctx_t ctx,
    const ring r,
    slong thread_limit)
{
    thread_pool_handle * handles;
    slong num_handles;

    /* get workers */
    handles = NULL;
    num_handles = 0;
    if (thread_limit > 1 && global_thread_pool_initialized)
    {
        slong max_num_handles = thread_pool_get_size(global_thread_pool);
        max_num_handles = FLINT_MIN(thread_limit - 1, max_num_handles);
        if (max_num_handles > 0)
        {
            handles = (thread_pool_handle *) flint_malloc(max_num_handles*sizeof(thread_pool_handle));
            num_handles = thread_pool_request(global_thread_pool, handles, max_num_handles);
        }
    }

    convert_nmod_mpoly_to_sing_base base(num_handles + 1, f, ctx, r);

    convert_nmod_mpoly_to_sing_arg * args = new convert_nmod_mpoly_to_sing_arg[base.num_threads];
    for (slong i = 0; i < base.num_threads; i++)
    {
        args[i].base = &base;
        args[i].start_idx = (i+0)*base.f->length/base.num_threads;
        args[i].end_idx   = (i+1)*base.f->length/base.num_threads;
        args[i].poly_end = NULL;
        args[i].poly_start = NULL;
    }

    /* construct pieces */
printf("constructing pieces\n");

    for (slong i = 0; i < num_handles; i++)
        thread_pool_wake(global_thread_pool, handles[i], convert_nmod_mpoly_to_sing_worker, args + i);
    convert_nmod_mpoly_to_sing_worker(args + num_handles);
    for (slong i = 0; i < num_handles; i++)
        thread_pool_wait(global_thread_pool, handles[i]);

printf("joining\n");

    /* join pieces */
    poly p = NULL;
    for (slong i = num_handles; i >= 0; i--)
    {
        if (args[i].poly_end != NULL)
        {
            pNext(args[i].poly_end) = p;
            p = args[i].poly_start;
        }
    }

    for (slong i = 0; i < num_handles; i++)
        thread_pool_give_back(global_thread_pool, handles[i]);
    if (handles)
        flint_free(handles);

    p_Test(p,r);
    return p;
}


poly Flint_Mult_MP(poly p, poly q, nmod_mpoly_ctx_t ctx, const ring r)
{
    nmod_mpoly_t pp, qq, res;
    convSingPFlintMP(pp, ctx, p, r, MPOLY_DEFAULT_THREAD_LIMIT);
    convSingPFlintMP(qq, ctx, q, r, MPOLY_DEFAULT_THREAD_LIMIT);
    nmod_mpoly_init(res, ctx);
    nmod_mpoly_mul(res, pp, qq, ctx);
    poly pres = convFlintMPSingP(res, ctx, r, MPOLY_DEFAULT_THREAD_LIMIT);
    nmod_mpoly_clear(res, ctx);
    nmod_mpoly_clear(pp, ctx);
    nmod_mpoly_clear(qq, ctx);
    nmod_mpoly_ctx_clear(ctx);
    p_Test(pres, r);
    return pres;
}
#endif
#endif
