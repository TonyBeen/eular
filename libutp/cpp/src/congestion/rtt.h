/*************************************************************************
    > File Name: rtt.h
    > Author: eular
    > Brief:
    > Created Time: Tue 09 Dec 2025 02:27:49 PM CST
 ************************************************************************/

#ifndef __CONGESTION_RTT_H__
#define __CONGESTION_RTT_H__

#include <stdint.h>
#include <math.h>

// RFC 6298
#define ALPHA_SHIFT 3   /* Alpha is 1/8 */
#define BETA_SHIFT  2   /* Beta is 1/4 */

namespace eular {
namespace utp {
class RttStats
{
public:
    RttStats() :
        m_srtt(0),
        m_rttvar(0),
        m_minrtt(0)
    {
    }

    ~RttStats() = default;

    /**
     * @brief 更新RTT估计值
     *
     * @param rtt 本次测量的往返时间，单位us
     */
    inline void update(int64_t measuredRtt)
    {
        if (m_srtt) {
            const int64_t srttUs = static_cast<int64_t>(m_srtt >> ALPHA_SHIFT);
            const int64_t delta = measuredRtt - srttUs;
            const uint64_t absDelta = static_cast<uint64_t>(std::abs(delta));

            // m_srtt stores the smoothed RTT in alpha-scaled form (x8).
            m_srtt = static_cast<uint64_t>(static_cast<int64_t>(m_srtt) + delta);

            m_rttvar -= (m_rttvar >> BETA_SHIFT); // m_rttvar * 3/4
            m_rttvar += (absDelta >> BETA_SHIFT); // m_rttvar + |delta| / 4
            if (measuredRtt < m_minrtt) {
                m_minrtt = static_cast<uint64_t>(measuredRtt);
            }
        } else {
            m_srtt = static_cast<uint64_t>(measuredRtt) << ALPHA_SHIFT;
            m_rttvar = static_cast<uint64_t>(measuredRtt) << (BETA_SHIFT - 1);
            m_minrtt = static_cast<uint64_t>(measuredRtt);
        }
    }

    inline uint64_t srtt() const { return m_srtt >> ALPHA_SHIFT; }
    inline uint64_t rttVar() const { return m_rttvar; }
    inline uint64_t minRTT() const { return m_minrtt; }

protected:
    uint64_t    m_srtt{0};      // smoothed round trip time
    uint64_t    m_rttvar{0};    // round trip time variation
    uint64_t    m_minrtt{0};    // minimum rtt
};

} // namespace utp
} // namespace eular

#endif // __CONGESTION_RTT_H__
