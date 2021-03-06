/*
    Copyright 2019-2020 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

#include "dma.h"
#include "core.h"

void Dma::transfer()
{
    for (int i = 0; i < 4; i++)
    {
        // Only transfer on active channels
        if (!(active & BIT(i)))
            continue;

        int dstAddrCnt = (dmaCnt[i] & 0x00600000) >> 21;
        int srcAddrCnt = (dmaCnt[i] & 0x01800000) >> 23;

        // Perform the transfer
        if (active & BIT(i + 4)) // Sound DMA
        {
            for (unsigned int j = 0; j < 4; j++)
            {
                // Transfer a word
                core->memory.write<uint32_t>(cpu, dstAddrs[i], core->memory.read<uint32_t>(cpu, srcAddrs[i]));

                // Adjust the source address
                if (srcAddrCnt == 0) // Increment
                    srcAddrs[i] += 4;
                else if (srcAddrCnt == 1) // Decrement
                    srcAddrs[i] -= 4;
            }
        }
        else if (dmaCnt[i] & BIT(26)) // Whole word transfer
        {
            for (unsigned int j = 0; j < wordCounts[i]; j++)
            {
                // Transfer a word
                core->memory.write<uint32_t>(cpu, dstAddrs[i], core->memory.read<uint32_t>(cpu, srcAddrs[i]));

                // Adjust the source address
                if (srcAddrCnt == 0) // Increment
                    srcAddrs[i] += 4;
                else if (srcAddrCnt == 1) // Decrement
                    srcAddrs[i] -= 4;

                // Adjust the destination address
                if (dstAddrCnt == 0 || dstAddrCnt == 3) // Increment
                    dstAddrs[i] += 4;
                else if (dstAddrCnt == 1) // Decrement
                    dstAddrs[i] -= 4;

                // In GXFIFO mode, only 112 words are sent at a time
                if (((dmaCnt[i] & 0x38000000) >> 27) == 7)
                {
                    gxFifoCount[i]++;
                    if (gxFifoCount[i] == 112)
                        break;
                }
            }
        }
        else // Half-word transfer
        {
            for (unsigned int j = 0; j < wordCounts[i]; j++)
            {
                // Transfer a half-word
                core->memory.write<uint16_t>(cpu, dstAddrs[i], core->memory.read<uint16_t>(cpu, srcAddrs[i]));

                // Adjust the source address
                if (srcAddrCnt == 0) // Increment
                    srcAddrs[i] += 2;
                else if (srcAddrCnt == 1) // Decrement
                    srcAddrs[i] -= 2;

                // Adjust the destination address
                if (dstAddrCnt == 0 || dstAddrCnt == 3) // Increment
                    dstAddrs[i] += 2;
                else if (dstAddrCnt == 1) // Decrement
                    dstAddrs[i] -= 2;

                // In GXFIFO mode, only 112 words are sent at a time
                if (((dmaCnt[i] & 0x38000000) >> 27) == 7)
                {
                    gxFifoCount[i]++;
                    if (gxFifoCount[i] == 112)
                        break;
                }
            }
        }

        // Don't end the GXFIFO transfer if there are still words left
        if (gxFifoCount[i] == 112)
        {
            wordCounts[i] -= 112;
            gxFifoCount[i] = 0;
            if (wordCounts[i] > 0)
                continue;
        }

        if (dmaCnt[i] & BIT(25)) // Repeat
        {
            // Reload the internal registers on repeat
            wordCounts[i] = dmaCnt[i] & 0x001FFFFF;
            if (dstAddrCnt == 3) // Increment and reload
                dstAddrs[i] = dmaDad[i];
        }
        else
        {
            // End the transfer
            dmaCnt[i] &= ~BIT(31);
            gxFifoCount[i] = 0;
        }

        // Trigger an end of transfer IRQ if enabled
        if (dmaCnt[i] & BIT(30))
            core->interpreter[cpu].sendInterrupt(8 + i);
    }

    // Some transfers are only triggered once at a time, so disable them after one transfer
    modes[1] = false;
    if (core->isGbaMode())
    {
        modes[4] = false;
        modes[8] = false;
        modes[9] = false;
    }
    else
    {
        modes[2] = false;
    }

    update();
}

void Dma::setMode(int mode, bool active)
{
    // Redirect ARM9 DMA modes on the ARM7 DMA
    if (cpu == 1)
    {
        switch (mode)
        {
            case 0: mode = 0; break; // Immediate
            case 1: mode = 2; break; // V-blank
            case 2: mode = 4; break; // H-blank (GBA)
            case 5: mode = 4; break; // DS cart (NDS)
        }
    }

    // Change the state of a DMA mode
    if (modes[mode] != active)
    {
        modes[mode] = active;
        update();
    }
}

void Dma::update()
{
    active = 0;

    // If a channel is enabled and its mode is active, set the channel as active
    for (int i = 0; i < 4; i++)
    {
        if ((dmaCnt[i] & BIT(31)) && modes[(dmaCnt[i] & 0x38000000) >> 27])
            active |= BIT(i);
    }

    // Handle special GBA modes
    if (core->isGbaMode())
    {
        for (int i = 1; i < 3; i++)
        {
            if ((dmaCnt[i] & BIT(31)) && ((dmaCnt[i] & 0x38000000) >> 27) == 6 &&
               ((dmaDad[i] == 0x40000A0 && modes[8]) || (dmaDad[i] == 0x40000A4 && modes[9])))
                active |= BIT(i) | BIT(i + 4);
        }
    }
}

void Dma::writeDmaSad(int channel, uint32_t mask, uint32_t value)
{
    // Write to one of the DMASAD registers
    mask &= ((cpu == 0 || channel != 0) ? 0x0FFFFFFF : 0x07FFFFFF);
    dmaSad[channel] = (dmaSad[channel] & ~mask) | (value & mask);
}

void Dma::writeDmaDad(int channel, uint32_t mask, uint32_t value)
{
    // Write to one of the DMADAD registers
    mask &= ((cpu == 0 || channel == 3) ? 0x0FFFFFFF : 0x07FFFFFF);
    dmaDad[channel] = (dmaDad[channel] & ~mask) | (value & mask);
}

void Dma::writeDmaCnt(int channel, uint32_t mask, uint32_t value)
{
    bool reload = false;

    // Reload the internal registers if the enable bit changes from 0 to 1
    if (!(dmaCnt[channel] & BIT(31)) && (value & BIT(31)))
        reload = true;

    // Write to one of the DMACNT registers
    mask &= ((cpu == 0) ? 0xFFFFFFFF : (channel == 3 ? 0xF7E0FFFF : 0xF7E03FFF));
    dmaCnt[channel] = (dmaCnt[channel] & ~mask) | (value & mask);

    // Reload the internal registers
    if (reload)
    {
        dstAddrs[channel] = dmaDad[channel];
        srcAddrs[channel] = dmaSad[channel];
        wordCounts[channel] = dmaCnt[channel] & 0x001FFFFF;
    }

    update();
}
