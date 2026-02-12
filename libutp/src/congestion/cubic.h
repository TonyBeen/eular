/*************************************************************************
    > File Name: cubic.h
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:22:43 PM CST
 ************************************************************************/

#ifndef __UTP_CONGESTION_CUBIC_H__
#define __UTP_CONGESTION_CUBIC_H__

#include "congestion/congestion.h"

namespace eular {
namespace utp {
class Cubic : public Congestion
{
public:
    Cubic(/* args */);
    ~Cubic();
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONGESTION_CUBIC_H__
