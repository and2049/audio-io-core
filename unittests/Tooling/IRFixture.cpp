extern "C" void apply_gain(const float *input, float *output, unsigned frames, float gain) {
  for (unsigned index = 0; index < frames; ++index) {
    output[index] = input[index] * gain;
  }
}
