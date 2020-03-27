// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include "seal/smallmodulus.h"
#include "seal/util/defines.h"
#include "seal/util/polyarith.h"
#include "seal/util/smallntt.h"
#include "seal/util/uintarith.h"
#include "seal/util/uintarithsmallmod.h"
#include <algorithm>

using namespace std;

namespace seal
{
    namespace util
    {
        SmallNTTTables::SmallNTTTables(int coeff_count_power, const SmallModulus &modulus, MemoryPoolHandle pool)
            : pool_(move(pool))
        {
#ifdef SEAL_DEBUG
            if (!pool_)
            {
                throw invalid_argument("pool is uninitialized");
            }
#endif
            if (!initialize(coeff_count_power, modulus))
            {
                // Generation failed; probably modulus wasn't prime.
                // It is necessary to check generated() after creating
                // this class.
            }
        }

        void SmallNTTTables::reset()
        {
            is_initialized_ = false;
            modulus_ = SmallModulus();
            root_ = 0;
            root_powers_.release();
            scaled_root_powers_.release();
            inv_root_powers_.release();
            scaled_inv_root_powers_.release();
            inv_root_powers_div_two_.release();
            scaled_inv_root_powers_div_two_.release();
            inv_degree_modulo_ = 0;
            coeff_count_power_ = 0;
            coeff_count_ = 0;
        }

        bool SmallNTTTables::initialize(int coeff_count_power, const SmallModulus &modulus)
        {
            reset();
#ifdef SEAL_DEBUG
            if ((coeff_count_power < get_power_of_two(SEAL_POLY_MOD_DEGREE_MIN)) ||
                coeff_count_power > get_power_of_two(SEAL_POLY_MOD_DEGREE_MAX))
            {
                throw invalid_argument("coeff_count_power out of range");
            }
#endif
            coeff_count_power_ = coeff_count_power;
            coeff_count_ = size_t(1) << coeff_count_power_;

            // Allocate memory for the tables
            root_powers_ = allocate_uint(coeff_count_, pool_);
            inv_root_powers_ = allocate_uint(coeff_count_, pool_);
            scaled_root_powers_ = allocate_uint(coeff_count_, pool_);
            scaled_inv_root_powers_ = allocate_uint(coeff_count_, pool_);
            inv_root_powers_div_two_ = allocate_uint(coeff_count_, pool_);
            scaled_inv_root_powers_div_two_ = allocate_uint(coeff_count_, pool_);
            modulus_ = modulus;

            // We defer parameter checking to try_minimal_primitive_root(...)
            if (!try_minimal_primitive_root(2 * coeff_count_, modulus_, root_))
            {
                reset();
                return false;
            }

            uint64_t inverse_root;
            if (!try_invert_uint_mod(root_, modulus_, inverse_root))
            {
                reset();
                return false;
            }

            // Populate the tables storing (scaled version of) powers of root
            // mod q in bit-scrambled order.
            ntt_powers_of_primitive_root(root_, root_powers_.get());
            ntt_scale_powers_of_primitive_root(root_powers_.get(), scaled_root_powers_.get());

            // Populate the tables storing (scaled version of) powers of
            // (root)^{-1} mod q in bit-scrambled order.
            ntt_powers_of_primitive_root(inverse_root, inv_root_powers_.get());
            ntt_scale_powers_of_primitive_root(inv_root_powers_.get(), scaled_inv_root_powers_.get());

            // Populate the tables storing (scaled version of ) 2 times
            // powers of roots^-1 mod q  in bit-scrambled order.
            for (size_t i = 0; i < coeff_count_; i++)
            {
                inv_root_powers_div_two_[i] = div2_uint_mod(inv_root_powers_[i], modulus_);
            }
            ntt_scale_powers_of_primitive_root(inv_root_powers_div_two_.get(), scaled_inv_root_powers_div_two_.get());

            // Reordering inv_root_powers_ so that the access pattern in inverse NTT is sequential.
            auto temp = allocate_uint(coeff_count_, pool_);
            uint64_t *temp_ptr = temp.get() + 1;
            for (size_t m = (coeff_count_ >> 1); m > 0; m >>= 1)
            {
                for (size_t i = 0; i < m; i++)
                {
                    *temp_ptr++ = inv_root_powers_[m + i];
                }
            }
            set_uint_uint(temp.get(), coeff_count_, inv_root_powers_.get());

            temp_ptr = temp.get() + 1;
            for (size_t m = (coeff_count_ >> 1); m > 0; m >>= 1)
            {
                for (size_t i = 0; i < m; i++)
                {
                    *temp_ptr++ = scaled_inv_root_powers_[m + i];
                }
            }
            set_uint_uint(temp.get(), coeff_count_, scaled_inv_root_powers_.get());

            // Last compute n^(-1) modulo q.
            uint64_t degree_uint = static_cast<uint64_t>(coeff_count_);
            is_initialized_ = try_invert_uint_mod(degree_uint, modulus_, inv_degree_modulo_);

            if (!is_initialized_)
            {
                reset();
                return false;
            }
            return true;
        }

        void SmallNTTTables::ntt_powers_of_primitive_root(uint64_t root, uint64_t *destination) const
        {
            uint64_t *destination_start = destination;
            *destination_start = 1;
            for (size_t i = 1; i < coeff_count_; i++)
            {
                uint64_t *next_destination = destination_start + reverse_bits(i, coeff_count_power_);
                *next_destination = multiply_uint_uint_mod(*destination, root, modulus_);
                destination = next_destination;
            }
        }

        // Compute floor (input * beta /q), where beta is a 64k power of 2 and  0 < q < beta.
        void SmallNTTTables::ntt_scale_powers_of_primitive_root(const uint64_t *input, uint64_t *destination) const
        {
            for (size_t i = 0; i < coeff_count_; i++, input++, destination++)
            {
                uint64_t wide_quotient[2]{ 0, 0 };
                uint64_t wide_coeff[2]{ 0, *input };
                divide_uint128_uint64_inplace(wide_coeff, modulus_.value(), wide_quotient);
                *destination = wide_quotient[0];
            }
        }

        /**
        This function computes in-place the negacyclic NTT. The input is
        a polynomial a of degree n in R_q, where n is assumed to be a power of
        2 and q is a prime such that q = 1 (mod 2n).

        The output is a vector A such that the following hold:
        A[j] =  a(psi**(2*bit_reverse(j) + 1)), 0 <= j < n.

        For details, see Michael Naehrig and Patrick Longa.
        */
        void ntt_negacyclic_harvey_lazy(uint64_t *operand, const SmallNTTTables &tables)
        {
#ifdef SEAL_DEBUG
            if (!tables)
            {
                throw std::logic_error("SmallNTTTables is uninitialized");
            }
#endif
            uint64_t modulus = tables.modulus().value();
            uint64_t two_times_modulus = modulus << 1;

            // Return the NTT in scrambled order
            size_t n = size_t(1) << tables.coeff_count_power();
            size_t t = n >> 1;
            for (size_t m = 1; m < n; m <<= 1)
            {
                size_t j1 = 0;
                if (t >= 4)
                {
                    for (size_t i = 0; i < m; i++)
                    {
                        size_t j2 = j1 + t;
                        const uint64_t W = tables.get_from_root_powers(m + i);
                        const uint64_t Wprime = tables.get_from_scaled_root_powers(m + i);

                        uint64_t *X = operand + j1;
                        uint64_t *Y = X + t;
                        uint64_t tx;
                        unsigned long long Q;
                        for (size_t j = j1; j < j2; j += 4)
                        {
                            tx = *X - (two_times_modulus &
                                          static_cast<uint64_t>(-static_cast<int64_t>(*X >= two_times_modulus)));
                            multiply_uint64_hw64(Wprime, *Y, &Q);
                            Q = *Y * W - Q * modulus;
                            *X++ = tx + Q;
                            *Y++ = tx + two_times_modulus - Q;

                            tx = *X - (two_times_modulus &
                                          static_cast<uint64_t>(-static_cast<int64_t>(*X >= two_times_modulus)));
                            multiply_uint64_hw64(Wprime, *Y, &Q);
                            Q = *Y * W - Q * modulus;
                            *X++ = tx + Q;
                            *Y++ = tx + two_times_modulus - Q;

                            tx = *X - (two_times_modulus &
                                          static_cast<uint64_t>(-static_cast<int64_t>(*X >= two_times_modulus)));
                            multiply_uint64_hw64(Wprime, *Y, &Q);
                            Q = *Y * W - Q * modulus;
                            *X++ = tx + Q;
                            *Y++ = tx + two_times_modulus - Q;

                            tx = *X - (two_times_modulus &
                                          static_cast<uint64_t>(-static_cast<int64_t>(*X >= two_times_modulus)));
                            multiply_uint64_hw64(Wprime, *Y, &Q);
                            Q = *Y * W - Q * modulus;
                            *X++ = tx + Q;
                            *Y++ = tx + two_times_modulus - Q;
                        }
                        j1 += (t << 1);
                    }
                }
                else
                {
                    for (size_t i = 0; i < m; i++)
                    {
                        size_t j2 = j1 + t;
                        const uint64_t W = tables.get_from_root_powers(m + i);
                        const uint64_t Wprime = tables.get_from_scaled_root_powers(m + i);

                        uint64_t *X = operand + j1;
                        uint64_t *Y = X + t;
                        uint64_t tx;
                        unsigned long long Q;
                        for (size_t j = j1; j < j2; j++)
                        {
                            // The Harvey butterfly: assume X, Y in [0, 2p), and return X', Y' in [0, 4p).
                            // X', Y' = X + WY, X - WY (mod p).
                            tx = *X - (two_times_modulus &
                                          static_cast<uint64_t>(-static_cast<int64_t>(*X >= two_times_modulus)));
                            multiply_uint64_hw64(Wprime, *Y, &Q);
                            Q = W * *Y - Q * modulus;
                            *X++ = tx + Q;
                            *Y++ = tx + two_times_modulus - Q;
                        }
                        j1 += (t << 1);
                    }
                }
                t >>= 1;
            }
        }

        // Inverse negacyclic NTT using Harvey's butterfly. (See Patrick Longa and Michael Naehrig).
        void inverse_ntt_negacyclic_harvey_lazy(uint64_t *operand, const SmallNTTTables &tables)
        {
#ifdef SEAL_DEBUG
            if (!tables)
            {
                throw std::logic_error("SmallNTTTables is uninitialized");
            }
#endif
            uint64_t modulus = tables.modulus().value();
            uint64_t two_times_modulus = modulus << 1;

            // return the bit-reversed order of NTT.
            size_t n = size_t(1) << tables.coeff_count_power();
            size_t t = 1;
            size_t root_index = 1;
            for (size_t m = (n >> 1); m > 1; m >>= 1)
            {
                size_t j1 = 0;
                if (t >= 4)
                {
                    for (size_t i = 0; i < m; i++, root_index++)
                    {
                        size_t j2 = j1 + t;
                        const uint64_t W = tables.get_from_inv_root_powers(root_index);
                        const uint64_t Wprime = tables.get_from_scaled_inv_root_powers(root_index);

                        uint64_t *X = operand + j1;
                        uint64_t *Y = X + t;
                        uint64_t tx;
                        uint64_t ty;
                        unsigned long long Q;
                        for (size_t j = j1; j < j2; j += 4)
                        {
                            tx = *X + *Y;
                            ty = *X + two_times_modulus - *Y;
                            *X++ = tx - (two_times_modulus &
                                     static_cast<uint64_t>(-static_cast<int64_t>(tx >= two_times_modulus)));
                            multiply_uint64_hw64(Wprime, ty, &Q);
                            *Y++ = ty * W - Q * modulus;

                            tx = *X + *Y;
                            ty = *X + two_times_modulus - *Y;
                            *X++ = tx - (two_times_modulus &
                                     static_cast<uint64_t>(-static_cast<int64_t>(tx >= two_times_modulus)));
                            multiply_uint64_hw64(Wprime, ty, &Q);
                            *Y++ = ty * W - Q * modulus;

                            tx = *X + *Y;
                            ty = *X + two_times_modulus - *Y;
                            *X++ = tx - (two_times_modulus &
                                     static_cast<uint64_t>(-static_cast<int64_t>(tx >= two_times_modulus)));
                            multiply_uint64_hw64(Wprime, ty, &Q);
                            *Y++ = ty * W - Q * modulus;

                            tx = *X + *Y;
                            ty = *X + two_times_modulus - *Y;
                            *X++ = tx - (two_times_modulus &
                                     static_cast<uint64_t>(-static_cast<int64_t>(tx >= two_times_modulus)));
                            multiply_uint64_hw64(Wprime, ty, &Q);
                            *Y++ = ty * W - Q * modulus;
                        }
                        j1 += (t << 1);
                    }
                }
                else
                {
                    for (size_t i = 0; i < m; i++, root_index++)
                    {
                        size_t j2 = j1 + t;
                        const uint64_t W = tables.get_from_inv_root_powers(root_index);
                        const uint64_t Wprime = tables.get_from_scaled_inv_root_powers(root_index);

                        uint64_t *X = operand + j1;
                        uint64_t *Y = X + t;
                        uint64_t tx;
                        uint64_t ty;
                        unsigned long long Q;
                        for (size_t j = j1; j < j2; j++)
                        {
                            tx = *X + *Y;
                            ty = *X + two_times_modulus - *Y;
                            *X++ = tx - (two_times_modulus &
                                     static_cast<uint64_t>(-static_cast<int64_t>(tx >= two_times_modulus)));
                            multiply_uint64_hw64(Wprime, ty, &Q);
                            *Y++ = ty * W - Q * modulus;
                        }
                        j1 += (t << 1);
                    }
                }
                t <<= 1;
            }

            const uint64_t inv_N = *(tables.get_inv_degree_modulo());
            const uint64_t W = tables.get_from_inv_root_powers(root_index);
            const uint64_t inv_N_W = multiply_uint_uint_mod(inv_N, W, tables.modulus());
            uint64_t wide_quotient[2]{ 0, 0 };
            uint64_t wide_coeff[2]{ 0, inv_N };
            divide_uint128_uint64_inplace(wide_coeff, modulus, wide_quotient);
            const uint64_t inv_Nprime = wide_quotient[0];
            wide_quotient[0] = 0;
            wide_quotient[1] = 0;
            wide_coeff[0] = 0;
            wide_coeff[1] = inv_N_W;
            divide_uint128_uint64_inplace(wide_coeff, modulus, wide_quotient);
            const uint64_t inv_N_Wprime = wide_quotient[0];

            uint64_t *X = operand;
            uint64_t *Y = X + (n >> 1);
            uint64_t tx;
            uint64_t ty;
            unsigned long long Q;
            for (size_t j = (n >> 1); j < n; j++)
            {
                tx = *X + *Y;
                tx -= two_times_modulus &
                            static_cast<uint64_t>(-static_cast<int64_t>(tx >= two_times_modulus));
                ty = *X + two_times_modulus - *Y;
                multiply_uint64_hw64(inv_Nprime, tx, &Q);
                *X++ = inv_N * tx - Q * modulus;
                multiply_uint64_hw64(inv_N_Wprime, ty, &Q);
                *Y++ = inv_N_W * ty - Q * modulus;
            }
        }
    } // namespace util
} // namespace seal
