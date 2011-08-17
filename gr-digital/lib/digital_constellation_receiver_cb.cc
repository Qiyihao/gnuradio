/* -*- c++ -*- */
/*
 * Copyright 2011 Free Software Foundation, Inc.
 *
 * This file is part of GNU Radio
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gr_io_signature.h>
#include <gr_prefs.h>
#include <digital_constellation_receiver_cb.h>
#include <stdexcept>
#include <gr_math.h>
#include <gr_expj.h>


#define M_TWOPI (2*M_PI)
#define VERBOSE_MM     0     // Used for debugging symbol timing loop
#define VERBOSE_COSTAS 0     // Used for debugging phase and frequency tracking

// Public constructor

digital_constellation_receiver_cb_sptr 
digital_make_constellation_receiver_cb(digital_constellation_sptr constell,
				       float loop_bw, float fmin, float fmax)
{
  return gnuradio::get_initial_sptr(new digital_constellation_receiver_cb (constell,
									   loop_bw,
									   fmin, fmax));
}
 
static int ios[] = {sizeof(char), sizeof(float), sizeof(float), sizeof(float)};
static std::vector<int> iosig(ios, ios+sizeof(ios)/sizeof(int));
digital_constellation_receiver_cb::digital_constellation_receiver_cb (digital_constellation_sptr constellation, 
								      float loop_bw, float fmin, float fmax)
  : gr_block ("constellation_receiver_cb",
	      gr_make_io_signature (1, 1, sizeof (gr_complex)),
	      gr_make_io_signaturev (1, 4, iosig)),
    d_freq(0), d_max_freq(fmax), d_min_freq(fmin), d_phase(0),
    d_constellation(constellation), 
    d_current_const_point(0)
{
  if (d_constellation->dimensionality() != 1)
    throw std::runtime_error ("This receiver only works with constellations of dimension 1.");

  // Set the damping factor for a critically damped system
  d_damping = sqrtf(2.0f)/2.0f;

  // Set the bandwidth, which will then call update_gains()
  set_loop_bandwidth(loop_bw);
}


/*******************************************************************
    SET FUNCTIONS
*******************************************************************/

void
digital_constellation_receiver_cb::set_loop_bandwidth(float bw) 
{
  if(bw < 0) {
    throw std::out_of_range ("digital_constellation_receiver_cb: invalid bandwidth. Must be >= 0.");
  }
  
  d_loop_bw = bw;
  update_gains();
}

void
digital_constellation_receiver_cb::set_damping_factor(float df) 
{
  if(df < 0 || df > 1.0) {
    throw std::out_of_range ("digital_constellation_receiver_cb: invalid damping factor. Must be in [0,1].");
  }
  
  d_damping = df;
  update_gains();
}

void
digital_constellation_receiver_cb::set_alpha(float alpha)
{
  if(alpha < 0 || alpha > 1.0) {
    throw std::out_of_range ("digital_constellation_receiver_cb: invalid alpha. Must be in [0,1].");
  }
  d_alpha = alpha;
}

void
digital_constellation_receiver_cb::set_beta(float beta)
{
  if(beta < 0 || beta > 1.0) {
    throw std::out_of_range ("digital_constellation_receiver_cb: invalid beta. Must be in [0,1].");
  }
  d_beta = beta;
}

void
digital_constellation_receiver_cb::set_frequency(float freq)
{
  if(freq > d_max_freq)
    d_freq = d_min_freq;
  else if(freq < d_min_freq)
    d_freq = d_max_freq;
  else
    d_freq = freq;
}

void
digital_constellation_receiver_cb::set_phase(float phase)
{
  d_phase = phase;
  while(d_phase>M_TWOPI)
    d_phase -= M_TWOPI;
  while(d_phase<-M_TWOPI)
    d_phase += M_TWOPI;
}

   
/*******************************************************************
    GET FUNCTIONS
*******************************************************************/


float
digital_constellation_receiver_cb::get_loop_bandwidth() const
{
  return d_loop_bw;
}

float
digital_constellation_receiver_cb::get_damping_factor() const
{
  return d_damping;
}

float
digital_constellation_receiver_cb::get_alpha() const
{
  return d_alpha;
}

float
digital_constellation_receiver_cb::get_beta() const
{
  return d_beta;
}

float
digital_constellation_receiver_cb::get_frequency() const
{
  return d_freq;
}

float
digital_constellation_receiver_cb::get_phase() const
{
  return d_phase;
}

/*******************************************************************
*******************************************************************/

void
digital_constellation_receiver_cb::update_gains()
{
  float denom = (1.0 + 2.0*d_damping*d_loop_bw + d_loop_bw*d_loop_bw);
  d_alpha = (4*d_damping*d_loop_bw) / denom;
  d_beta = (4*d_loop_bw*d_loop_bw) / denom;
}

void
digital_constellation_receiver_cb::phase_error_tracking(float phase_error)
{
  d_freq += d_beta*phase_error;             // adjust frequency based on error
  d_phase += d_freq + d_alpha*phase_error;  // adjust phase based on error

  // Make sure we stay within +-2pi
  while(d_phase > M_TWOPI)
    d_phase -= M_TWOPI;
  while(d_phase < -M_TWOPI)
    d_phase += M_TWOPI;
  
  // Limit the frequency range
  d_freq = gr_branchless_clip(d_freq, d_max_freq);
  
#if VERBOSE_COSTAS
  printf("cl: phase_error: %f  phase: %f  freq: %f  sample: %f+j%f  constellation: %f+j%f\n",
	 phase_error, d_phase, d_freq, sample.real(), sample.imag(), 
	 d_constellation->points()[d_current_const_point].real(),
	 d_constellation->points()[d_current_const_point].imag());
#endif
}

int
digital_constellation_receiver_cb::general_work (int noutput_items,
						 gr_vector_int &ninput_items,
						 gr_vector_const_void_star &input_items,
						 gr_vector_void_star &output_items)
{
  const gr_complex *in = (const gr_complex *) input_items[0];
  unsigned char *out = (unsigned char *) output_items[0];

  int i=0;

  float phase_error;
  unsigned int sym_value;
  gr_complex sample, nco;

  float *out_err = 0, *out_phase = 0, *out_freq = 0;
  if(output_items.size() == 4) {
    out_err = (float *) output_items[1];
    out_phase = (float *) output_items[2];
    out_freq = (float *) output_items[3];
  }

  while((i < noutput_items) && (i < ninput_items[0])) {
    sample = in[i];
    nco = gr_expj(d_phase);   // get the NCO value for derotating the current sample
    sample = nco*sample;      // get the downconverted symbol
    sym_value = d_constellation->decision_maker_pe(&sample, &phase_error);
    //    phase_error = -arg(sample*conj(d_constellation->points()[sym_value]));
    phase_error_tracking(phase_error);  // corrects phase and frequency offsets
    out[i] = sym_value;
    if(output_items.size() == 4) {
      out_err[i] = phase_error;
      out_phase[i] = d_phase;
      out_freq[i] = d_freq;
    }
    i++;
  }

  consume_each(i);
  return i;
}

