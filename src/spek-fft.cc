#include <cmath>

#define __STDC_CONSTANT_MACROS
extern "C" {
#include <libavutil/tx.h>
}

#include "spek-fft.h"

class FFTPlanImpl : public FFTPlan
{
public:
    FFTPlanImpl(int nbits);
    ~FFTPlanImpl() override;

    void execute() override;

private:
    AVTXContext *cx;
    av_tx_fn tx;
    AVComplexFloat *tmp;
};

std::unique_ptr<FFTPlan> FFT::create(int nbits)
{
    return std::unique_ptr<FFTPlan>(new FFTPlanImpl(nbits));
}

FFTPlanImpl::FFTPlanImpl(int nbits) : FFTPlan(nbits), cx(nullptr), tx(nullptr), tmp(nullptr)
{
    const float scale = 1.0f;
    const int n = this->get_input_size();
    // Forward real-to-complex DFT, unscaled, matching the old av_rdft DFT_R2C.
    if (av_tx_init(&this->cx, &this->tx, AV_TX_FLOAT_RDFT, 0, n, &scale, 0) < 0) {
        this->cx = nullptr;
        this->tx = nullptr;
    }
    // av_tx outputs N/2+1 complex bins into a separate, aligned buffer.
    this->tmp = (AVComplexFloat*) av_malloc(sizeof(AVComplexFloat) * (n / 2 + 1));
}

FFTPlanImpl::~FFTPlanImpl()
{
    av_tx_uninit(&this->cx);
    av_freep(&this->tmp);
}

void FFTPlanImpl::execute()
{
    if (!this->tx || !this->tmp) {
        return;
    }
    this->tx(this->cx, this->tmp, this->get_input(), sizeof(float));

    // Calculate magnitudes. Unlike av_rdft, av_tx does not pack the Nyquist
    // bin into the imaginary part of the DC bin: bins 0..N/2 are all explicit.
    int n = this->get_input_size();
    float n2 = n * n;
    for (int i = 0; i <= n / 2; i++) {
        float re = this->tmp[i].re;
        float im = this->tmp[i].im;
        this->set_output(i, 10.0f * log10f((re * re + im * im) / n2));
    }
}
