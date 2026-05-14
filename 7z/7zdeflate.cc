#include "7z.h"

#include "../vendor/7z2601/CPP/Common/MyInitGuid.h"
#include "../vendor/7z2601/CPP/7zip/Compress/DeflateEncoder.h"
#include "../vendor/7z2601/CPP/7zip/Compress/DeflateDecoder.h"
#include "../vendor/7z2601/CPP/7zip/Common/StreamObjects.h"

#include "zlib.h"

bool compress_deflate_7z(const unsigned char* in_data, unsigned in_size, unsigned char* out_data, unsigned& out_size, unsigned num_passes, unsigned num_fast_bytes) throw ()
{
	try {
		NCompress::NDeflate::NEncoder::CCoder cc;

		const PROPID prop_ids[2] = {
			NCoderPropID::kNumPasses,
			NCoderPropID::kNumFastBytes
		};
		PROPVARIANT props[2];
		memset(props, 0, sizeof(props));
		props[0].vt = VT_UI4;
		props[0].ulVal = num_passes;
		props[1].vt = VT_UI4;
		props[1].ulVal = num_fast_bytes;
		if (cc.BaseSetEncoderProperties2(prop_ids, props, 2) != S_OK)
			return false;

		CBufInStream in;
		in.Init(in_data, in_size);
		CBufPtrSeqOutStream out;
		out.Init(out_data, out_size);

		UInt64 in_size_l = in_size;
		if (cc.BaseCode(&in, &out, &in_size_l, 0, 0) != S_OK)
			return false;
		if (!in.WasFinished())
			return false;
		if (out.GetPos() > out_size)
			return false;

		out_size = static_cast<unsigned>(out.GetPos());

		return true;
	} catch (...) {
		return false;
	}
}

bool decompress_deflate_7z(const unsigned char* in_data, unsigned in_size, unsigned char* out_data, unsigned out_size) throw () {
	try {
		NCompress::NDeflate::NDecoder::CCOMCoder cc;
		CBufInStream in;
		in.Init(in_data, in_size);
		CBufPtrSeqOutStream out;
		out.Init(out_data, out_size);

		UInt64 in_size_l = in_size;
		UInt64 out_size_l = out_size;

		ICompressCoder* coder = &cc;
		if (coder->Code(&in, &out, &in_size_l, &out_size_l, 0) != S_OK)
			return false;

		if (out.GetPos() != out_size)
			return false;

		return true;
	} catch (...) {
		return false;
	}
}

bool compress_rfc1950_7z(const unsigned char* in_data, unsigned in_size, unsigned char* out_data, unsigned& out_size, unsigned num_passes, unsigned num_fast_bytes) throw ()
{
	if (out_size < 6)
		return false;

	// 8 - deflate
	// 7 - 32k window
	// 3 - max compression
	unsigned header = (8 << 8) | (7 << 12) | (3 << 6);

	header += 31 - (header % 31);

	out_data[0] = (header >> 8) & 0xFF;
	out_data[1] = header & 0xFF;
	out_data += 2;

	unsigned size = out_size - 6;
	if (!compress_deflate_7z(in_data, in_size, out_data, size, num_passes, num_fast_bytes)) {
		return false;
	}
	out_data += size;

	unsigned adler = adler32(adler32(0,0,0), in_data, in_size);

	out_data[0] = (adler >> 24) & 0xFF;
	out_data[1] = (adler >> 16) & 0xFF;
	out_data[2] = (adler >> 8) & 0xFF;
	out_data[3] = adler & 0xFF;

	out_size = size + 6;

	return true;
}
