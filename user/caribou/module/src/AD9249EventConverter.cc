#include "CaribouEvent2StdEventConverter.hh"

#include "utils/log.hpp"

#include <algorithm>
#include <fstream>
#include "TF1.h"

using namespace eudaq;

namespace {
auto dummy0 = eudaq::Factory<eudaq::StdEventConverter>::Register<
    AD9249Event2StdEventConverter>(AD9249Event2StdEventConverter::m_id_factory);
}

size_t AD9249Event2StdEventConverter::trig_(0);
bool AD9249Event2StdEventConverter::m_configured(0);
int64_t AD9249Event2StdEventConverter::m_runStartTime(-1);
std::string AD9249Event2StdEventConverter::m_waveform_filename("");
std::ofstream AD9249Event2StdEventConverter::m_outfile_waveforms;
// baseline evaluation
int AD9249Event2StdEventConverter::m_blStart(150);
int AD9249Event2StdEventConverter::m_blEnd(80);
// amplitude evaluation
int AD9249Event2StdEventConverter::m_ampStart(170);
int AD9249Event2StdEventConverter::m_ampEnd(270);
// calibration functions
double AD9249Event2StdEventConverter::m_calib_range_min(0);
double AD9249Event2StdEventConverter::m_calib_range_max(16384);
std::vector<std::string>
AD9249Event2StdEventConverter::m_calib_strings(16, "x");
std::vector<TF1>
AD9249Event2StdEventConverter::m_calib_functions(16, TF1());

void AD9249Event2StdEventConverter::decodeChannel(
    const size_t adc, const std::vector<uint8_t> &data, size_t size,
    size_t offset, std::vector<std::vector<uint16_t> > &waveforms,
    uint64_t &timestamp) const {

  // Timestamp index
  size_t ts_i = 0;

  for (size_t i = offset; i < offset + size; i += 2) {
    // Channel is ADC half times channels plus channel number within data block
    size_t ch = adc * 8 + ((i - offset) / 2) % 8;

    // Get waveform data
    uint16_t val =
        data.at(i) + ((static_cast<uint16_t>(data.at(i + 1)) & 0x3F) << 8);
    waveforms.at(ch).push_back(val);

    // If we have a full timestamp, skip the rest:
    if (ts_i >= 28) {
      continue;
    }

    // Treat timestamp data
    uint64_t ts = (data.at(i + 1) >> 6);

    // Channel 7 (or 15) have status bits only:
    if (ch == adc * 8 + 7) {
      // Check if this is a timestamp start - if not, reset timestamp index to
      // zero:
      if (ts_i < 8 && (ts & 0x1) == 0) {
        ts_i = 0;
      }
    } else {
      timestamp += (ts << 2 * ts_i);
      ts_i++;
    }
  }

  // Convert timestamp to picoseconds from the 65MHz clock (~15ns cycle):
  //  timestamp *= static_cast<uint64_t>(1. / 65. * 1e6);
  timestamp = static_cast<uint64_t>(timestamp * 1e6 / 65.);
}

bool
AD9249Event2StdEventConverter::Converting(eudaq::EventSPC d1,
                                          eudaq::StandardEventSP d2,
                                          eudaq::ConfigurationSPC conf) const {

  if (!m_configured) {
    m_blStart = conf->Get("blStart", m_blStart);
    m_blEnd = conf->Get("blEnd", m_blEnd);
    m_ampStart = conf->Get("ampStart", m_ampStart);
    m_ampEnd = conf->Get("ampEnd", m_ampEnd);
    m_calib_range_min = conf->Get("calib_range_min", m_calib_range_min);
    m_calib_range_max = conf->Get("calib_range_max", m_calib_range_max);
    m_waveform_filename = conf->Get("waveform_filename", "");

    // read calibration functions
    m_calib_functions.clear();
    for (unsigned int i = 0; i < m_calib_strings.size(); i++) {
      std::string name = "calibration_px" + to_string(mapping.at(i).first) +
                         to_string(mapping.at(i).second);
      m_calib_strings.at(i) = conf->Get(name, m_calib_strings.at(i));
      m_calib_functions.emplace_back(name.c_str(), m_calib_strings.at(i).c_str(),
                                     m_calib_range_min, m_calib_range_max);
    }

    EUDAQ_DEBUG("Using configuration:");
    EUDAQ_DEBUG(" blStart   = " + to_string(m_blStart));
    EUDAQ_DEBUG(" blEnd     = " + to_string(m_blEnd));
    EUDAQ_DEBUG(" ampStart  = " + to_string(m_ampStart));
    EUDAQ_DEBUG(" ampEnd    = " + to_string(m_ampStart));
    EUDAQ_DEBUG(" calib_range_min = " + to_string(m_calib_range_min));
    EUDAQ_DEBUG(" calib_range_max = " + to_string(m_calib_range_max));
    EUDAQ_DEBUG("Calibration functions: ");
    if(EUDAQ_IS_LOGGED("DEBUG")){
      for (unsigned int i = 0; i < m_calib_strings.size(); i++) {
        EUDAQ_DEBUG(to_string(m_calib_functions.at(i).GetName()) + " " +
                    to_string(m_calib_functions.at(i).GetExpFormula()));
      }
    }
    EUDAQ_DEBUG(" calib_range_min  = " + to_string(m_calib_range_min));
    EUDAQ_DEBUG(" calib_range_max  = " + to_string(m_calib_range_max));

    m_configured = true;
  }
  auto ev = std::dynamic_pointer_cast<const eudaq::RawEvent>(d1);
  EUDAQ_DEBUG("Decoding AD event " + to_string(ev->GetEventN()) + " trig " +
              to_string(trig_));

  const size_t header_offset = 8;
  auto datablock0 = ev->GetBlock(0);

  // std::ofstream out;
  // out.open("/tmp/out.dat", std::ofstream::out | std::ofstream::binary |
  // std::ofstream::app); out.write(reinterpret_cast<char*>(datablock0.data()),
  // datablock0.size()); out.close();

  // Get configured burst length from header:
  uint32_t burst_length =
      (static_cast<uint32_t>(datablock0.at(3)) << 8) | datablock0.at(2);

  // Check total available data against expected event size:
  const size_t evt_length = burst_length * 128 * 2 * 16 + 16;
  if (datablock0.size() < evt_length) {
    // FIXME throw something at someone?
    // std::cout << "Event length " << datablock0.size() << " not enough for
    // full event, requires " << evt_length << std::endl;
    return false;
  }

  EUDAQ_DEBUG("Burst: " + to_string(burst_length));

  // Read waveforms
  std::vector<std::vector<uint16_t> > waveforms;
  waveforms.resize(16);

  uint32_t size_ADC0 = (static_cast<uint32_t>(datablock0.at(7)) << 24) +
                       (static_cast<uint32_t>(datablock0.at(6)) << 16) +
                       (static_cast<uint32_t>(datablock0.at(5)) << 8) +
                       (datablock0.at(4) << 0);
  uint32_t size_ADC1 =
      (static_cast<uint32_t>(datablock0.at(header_offset + size_ADC0 + 7))
       << 24) +
      (static_cast<uint32_t>(datablock0.at(header_offset + size_ADC0 + 6))
       << 16) +
      (static_cast<uint32_t>(datablock0.at(header_offset + size_ADC0 + 5))
       << 8) +
      (datablock0.at(header_offset + size_ADC0 + 4) << 0);

  // Decode channels:
  uint64_t timestamp0 = 0;
  uint64_t timestamp1 = 0;
  decodeChannel(0, datablock0, size_ADC0, header_offset, waveforms, timestamp0);
  decodeChannel(1, datablock0, size_ADC1, 2 * header_offset + size_ADC0,
                waveforms, timestamp1);

  // store time of the run start
  if (trig_ <= 1) {
    m_runStartTime = timestamp0; // just use one of them for now
  }

  // Prepare output plane:
  eudaq::StandardPlane plane(0, "Caribou", "AD9249");
  plane.SetSizeZS(4, 4, 0);

  // print waveforms to file, if a filename is given
  // this returns false! If you want to change that that remove `trig_++`!!!
  if (!m_waveform_filename.empty()) {

    // Open
    m_outfile_waveforms.open(m_waveform_filename, std::ios_base::app); // append

    // Print to file
    for (size_t ch = 0; ch < waveforms.size(); ch++) {
      m_outfile_waveforms << trig_ << " " << ch << " " << mapping.at(ch).first
                          << " " << mapping.at(ch).second << " : ";
      auto const &waveform = waveforms.at(ch);
      for (auto const &sample : waveform) {
        m_outfile_waveforms << sample << " ";
      }
      m_outfile_waveforms << std::endl;
    }

    m_outfile_waveforms.close();
    trig_++;
    return false;
  }

  EUDAQ_DEBUG("_______________ Event " + to_string(ev->GetEventN()) + " trig " +
              to_string(trig_) + " __________");

  for (size_t ch = 0; ch < waveforms.size(); ch++) {

    // find waveform maximum
    auto max = std::max_element(waveforms[ch].begin()+m_ampStart, waveforms[ch].begin()+m_ampEnd);
    auto max_posizion = std::distance(waveforms[ch].begin(), max);

    // this means that we will not have an amplitude for some noise events.
    if ((max_posizion - m_blStart) < 0) {
      EUDAQ_DEBUG("  Skipping channel " + to_string(ch) + " max too early");
      continue;
    }

    // calculate waveform baseline
    double baseline = 0.;
    for (int i = max_posizion - m_blStart; i < max_posizion - m_blEnd; i++) {
      baseline += waveforms[ch][i];
    }
    baseline /= m_blStart - m_blEnd;

    // calculate amplitude and apply calibration
    double amplitude = m_calib_functions.at(ch).Eval(*max - baseline);
    if (amplitude > m_calib_range_max)
      amplitude = m_calib_range_max;
    if (amplitude < m_calib_range_min)
      amplitude = 0;

    plane.PushPixel(mapping.at(ch).first, mapping.at(ch).second,amplitude,timestamp0);
  }

  // Add the plane to the StandardEvent
  d2->AddPlane(plane);

  d2->SetTimeBegin(timestamp0 - m_runStartTime);
  d2->SetTimeEnd(timestamp0 - m_runStartTime);
  d2->SetTriggerN(trig_);
  trig_++;

  // Identify the detetor type
  d2->SetDetectorType("AD9249");
  // Indicate that data was successfully converted
  return true;
}

/*
 *  Erics python reference
 *
channels = 8

while True:
    h = file.read(4)
    header = struct.unpack('HH', h)
    bursts = header[1]
    points = 128 * bursts
    print("Channel", header[0], "Burst", header[1])

    s = file.read(4)
    size = struct.unpack('I', s)[0]
    print("Block size", size)

    while size > 0:
        data = file.read(points*2*channels)
        print("Reading", points*2*channels, "bytes")
        size -= points*2*channels

        val = [(i[0] & 0x3FFF) for i in struct.iter_unpack('<H', data)]
        val2 = np.reshape(val, (channels, -1), order='F')

        aux = [(i[0] >> 14) for i in struct.iter_unpack('<H', data)]
        aux2 = np.reshape(aux, (-1, channels))

        foo = []

        for i in aux2:
            if i[-1] & 2:
                print('trigger')

            if i[-1] & 1:
                out = 0
                for j in foo[::-1]:
                    out <<= 2
                    out |= j
                print(out/65000000.0)
                foo = []
            foo.extend(i[:-1])


        #fig, ax = plt.subplots(2,4, figsize=(16,9), sharex='col', sharey='row')
        fig, ax = plt.subplots(2,4, figsize=(16,9), sharex='all', sharey='all')
        for x in range(0, 4):
            for y in range(0, 2):
                i = y*4+x
                channel = i + 8*header[0]
                ax[y][x].plot(np.arange(0, len(val2[i]))*(1.0/65), val2[i])
                ax[y][x].set_title('ch {}'.format(channel))

        plt.show()

 */
