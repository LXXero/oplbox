/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* OPLBOX: the OPL2 fast path over Nuked-OPL3 (opl3.c). Included by oplbox.c and by the host harness. */
/* 18 steps of the OPL noise LFSR (23 bits, taps 14 and 0) in one go: the map is linear over GF(2), so the
 * result is the XOR of three byte-indexed tables. Built and verified against stepping by tools/lfsr_tables.py. */
static const uint32_t NOISE18[3][256] = {
    {0,16416,32832,49248,65664,82080,98496,114912,131328,147744,164160,180576,196992,213408,229824,246240,262656,279072,295488,311904,328320,344736,361152,377568,393984,410400,426816,443232,459648,476064,492480,508896,525312,541728,558144,574560,590976,607392,623808,640224,656640,673056,689472,705888,722304,738720,755136,771552,787968,804384,820800,837216,853632,870048,886464,902880,919296,935712,952128,968544,984960,1001376,1017792,1034208,1050624,1067040,1083456,1099872,1116288,1132704,1149120,1165536,1181952,1198368,1214784,1231200,1247616,1264032,1280448,1296864,1313280,1329696,1346112,1362528,1378944,1395360,1411776,1428192,1444608,1461024,1477440,1493856,1510272,1526688,1543104,1559520,1575936,1592352,1608768,1625184,1641600,1658016,1674432,1690848,1707264,1723680,1740096,1756512,1772928,1789344,1805760,1822176,1838592,1855008,1871424,1887840,1904256,1920672,1937088,1953504,1969920,1986336,2002752,2019168,2035584,2052000,2068416,2084832,2101248,2117664,2134080,2150496,2166912,2183328,2199744,2216160,2232576,2248992,2265408,2281824,2298240,2314656,2331072,2347488,2363904,2380320,2396736,2413152,2429568,2445984,2462400,2478816,2495232,2511648,2528064,2544480,2560896,2577312,2593728,2610144,2626560,2642976,2659392,2675808,2692224,2708640,2725056,2741472,2757888,2774304,2790720,2807136,2823552,2839968,2856384,2872800,2889216,2905632,2922048,2938464,2954880,2971296,2987712,3004128,3020544,3036960,3053376,3069792,3086208,3102624,3119040,3135456,3151872,3168288,3184704,3201120,3217536,3233952,3250368,3266784,3283200,3299616,3316032,3332448,3348864,3365280,3381696,3398112,3414528,3430944,3447360,3463776,3480192,3496608,3513024,3529440,3545856,3562272,3578688,3595104,3611520,3627936,3644352,3660768,3677184,3693600,3710016,3726432,3742848,3759264,3775680,3792096,3808512,3824928,3841344,3857760,3874176,3890592,3907008,3923424,3939840,3956256,3972672,3989088,4005504,4021920,4038336,4054752,4071168,4087584,4104000,4120416,4136832,4153248,4169664,4186080},
    {0,4202496,16384,4218880,32768,4235264,49152,4251648,65536,4268032,81920,4284416,98304,4300800,114688,4317184,131072,4333568,147456,4349952,163840,4366336,180224,4382720,196608,4399104,212992,4415488,229376,4431872,245760,4448256,262144,4464640,278528,4481024,294912,4497408,311296,4513792,327680,4530176,344064,4546560,360448,4562944,376832,4579328,393216,4595712,409600,4612096,425984,4628480,442368,4644864,458752,4661248,475136,4677632,491520,4694016,507904,4710400,540704,4743200,524320,4726816,573472,4775968,557088,4759584,606240,4808736,589856,4792352,639008,4841504,622624,4825120,671776,4874272,655392,4857888,704544,4907040,688160,4890656,737312,4939808,720928,4923424,770080,4972576,753696,4956192,802848,5005344,786464,4988960,835616,5038112,819232,5021728,868384,5070880,852000,5054496,901152,5103648,884768,5087264,933920,5136416,917536,5120032,966688,5169184,950304,5152800,999456,5201952,983072,5185568,1032224,5234720,1015840,5218336,1081408,5283904,1097792,5300288,1048640,5251136,1065024,5267520,1146944,5349440,1163328,5365824,1114176,5316672,1130560,5333056,1212480,5414976,1228864,5431360,1179712,5382208,1196096,5398592,1278016,5480512,1294400,5496896,1245248,5447744,1261632,5464128,1343552,5546048,1359936,5562432,1310784,5513280,1327168,5529664,1409088,5611584,1425472,5627968,1376320,5578816,1392704,5595200,1474624,5677120,1491008,5693504,1441856,5644352,1458240,5660736,1540160,5742656,1556544,5759040,1507392,5709888,1523776,5726272,1622112,5824608,1605728,5808224,1589344,5791840,1572960,5775456,1687648,5890144,1671264,5873760,1654880,5857376,1638496,5840992,1753184,5955680,1736800,5939296,1720416,5922912,1704032,5906528,1818720,6021216,1802336,6004832,1785952,5988448,1769568,5972064,1884256,6086752,1867872,6070368,1851488,6053984,1835104,6037600,1949792,6152288,1933408,6135904,1917024,6119520,1900640,6103136,2015328,6217824,1998944,6201440,1982560,6185056,1966176,6168672,2080864,6283360,2064480,6266976,2048096,6250592,2031712,6234208},
    {0,2162816,4325632,6488448,262657,2425473,4588289,6751105,525314,2688130,4850946,7013762,787971,2950787,5113603,7276419,1050628,3213444,5376260,7539076,1313285,3476101,5638917,7801733,1575942,3738758,5901574,8064390,1838599,4001415,6164231,8327047,2101256,69768,6426888,4395400,2363913,332425,6689545,4658057,2626570,595082,6952202,4920714,2889227,857739,7214859,5183371,3151884,1120396,7477516,5446028,3414541,1383053,7740173,5708685,3677198,1645710,8002830,5971342,3939855,1908367,8265487,6233999,4202512,6365328,139536,2302352,4465169,6627985,402193,2565009,4727826,6890642,664850,2827666,4990483,7153299,927507,3090323,5253140,7415956,1190164,3352980,5515797,7678613,1452821,3615637,5778454,7941270,1715478,3878294,6041111,8203927,1978135,4140951,6303768,4272280,2240792,209304,6566425,4534937,2503449,471961,6829082,4797594,2766106,734618,7091739,5060251,3028763,997275,7354396,5322908,3291420,1259932,7617053,5585565,3554077,1522589,7879710,5848222,3816734,1785246,8142367,6110879,4079391,2047903,0,2162816,4325632,6488448,262657,2425473,4588289,6751105,525314,2688130,4850946,7013762,787971,2950787,5113603,7276419,1050628,3213444,5376260,7539076,1313285,3476101,5638917,7801733,1575942,3738758,5901574,8064390,1838599,4001415,6164231,8327047,2101256,69768,6426888,4395400,2363913,332425,6689545,4658057,2626570,595082,6952202,4920714,2889227,857739,7214859,5183371,3151884,1120396,7477516,5446028,3414541,1383053,7740173,5708685,3677198,1645710,8002830,5971342,3939855,1908367,8265487,6233999,4202512,6365328,139536,2302352,4465169,6627985,402193,2565009,4727826,6890642,664850,2827666,4990483,7153299,927507,3090323,5253140,7415956,1190164,3352980,5515797,7678613,1452821,3615637,5778454,7941270,1715478,3878294,6041111,8203927,1978135,4140951,6303768,4272280,2240792,209304,6566425,4534937,2503449,471961,6829082,4797594,2766106,734618,7091739,5060251,3028763,997275,7354396,5322908,3291420,1259932,7617053,5585565,3554077,1522589,7879710,5848222,3816734,1785246,8142367,6110879,4079391,2047903},
};
#define NOISE_JUMP18(c) ((c)->noise = NOISE18[0][(c)->noise & 0xffu] ^ NOISE18[1][((c)->noise >> 8) & 0xffu] ^ NOISE18[2][((c)->noise >> 16) & 0x7fu])
#define NOISE_STEP(c) do { uint32_t nz = (c)->noise; (c)->noise = (nz >> 1) | ((((nz >> 14) ^ nz) & 1u) << 22); } while (0)
#ifndef OPL_NO_SKIP
#define OPL_NO_SKIP 0
#endif

/* One slot, one sample: Nuked-OPL3's OPL3_ProcessSlot (SlotCalcFB, EnvelopeCalc, PhaseGenerate, SlotGenerate)
 * line for line, with the values that only change on a register write taken from the slot's caches
 * (opl3.c: OPL3_SlotCacheRates / OPL3_SlotCacheInc / tlksl) instead of being recomputed every sample. */
static __attribute__((noinline)) void opl2_slot(opl3_slot *slot)
{
    opl3_chip *chip = slot->chip;
    uint8_t reset = 0, rate_hi = 0, rate_lo = 0, nonzero = 0, eg_shift, shift = 0, eg_off = 0;
    uint16_t eg_rout;
    int16_t eg_inc = 0;
    int ri = -1;
    uint32_t inc;
    uint16_t phase;
    uint32_t noise;

    /* feedback */
    if (slot->channel->fb != 0x00)
        slot->fbmod = (slot->prout + slot->out) >> (0x09 - slot->channel->fb);
    else
        slot->fbmod = 0;
    slot->prout = slot->out;

    /* envelope */
    slot->eg_out = (uint16_t)(slot->eg_rout + slot->tlksl + *slot->trem);
    if (slot->key && slot->eg_gen == envelope_gen_num_release) {
        reset = 1; ri = 0;
    } else {
        switch (slot->eg_gen) {
        case envelope_gen_num_attack:  ri = 0; break;
        case envelope_gen_num_decay:   ri = 1; break;
        case envelope_gen_num_sustain: if (!slot->reg_type) ri = 2; break;
        case envelope_gen_num_release: ri = 2; break;
        }
    }
    slot->pg_reset = reset;
    if (ri >= 0) { rate_hi = slot->rate_hi[ri]; rate_lo = slot->rate_lo[ri]; nonzero = slot->rate_nz[ri]; }
    /* (ri < 0: a sustaining envelope at its sustain level; reg_rate 0 -> nonzero 0, the rate is never used) */
    eg_shift = (uint8_t)(rate_hi + chip->eg_add);
    if (nonzero) {
        if (rate_hi < 12) {
            if (chip->eg_state) {
                switch (eg_shift) {
                case 12: shift = 1; break;
                case 13: shift = (rate_lo >> 1) & 0x01; break;
                case 14: shift = rate_lo & 0x01; break;
                default: break;
                }
            }
        } else {
            shift = (uint8_t)((rate_hi & 0x03) + eg_incstep[rate_lo][chip->eg_timer_lo]);
            if (shift & 0x04) shift = 0x03;
            if (!shift) shift = chip->eg_state;
        }
    }
    eg_rout = slot->eg_rout;
    if (reset && rate_hi == 0x0f) eg_rout = 0x00;                    /* instant attack */
    if ((slot->eg_rout & 0x1f8) == 0x1f8) eg_off = 1;                 /* envelope off */
    if (slot->eg_gen != envelope_gen_num_attack && !reset && eg_off) eg_rout = 0x1ff;
    switch (slot->eg_gen) {
    case envelope_gen_num_attack:
        if (!slot->eg_rout) slot->eg_gen = envelope_gen_num_decay;
        else if (slot->key && shift > 0 && rate_hi != 0x0f) eg_inc = (int16_t)(~slot->eg_rout >> (4 - shift));
        break;
    case envelope_gen_num_decay:
        if ((slot->eg_rout >> 4) == slot->reg_sl) slot->eg_gen = envelope_gen_num_sustain;
        else if (!eg_off && !reset && shift > 0) eg_inc = (int16_t)(1 << (shift - 1));
        break;
    case envelope_gen_num_sustain:
    case envelope_gen_num_release:
        if (!eg_off && !reset && shift > 0) eg_inc = (int16_t)(1 << (shift - 1));
        break;
    }
    slot->eg_rout = (uint16_t)((eg_rout + eg_inc) & 0x1ff);
    if (reset) slot->eg_gen = envelope_gen_num_attack;
    if (!slot->key) slot->eg_gen = envelope_gen_num_release;

    /* phase */
    if (slot->reg_vib) {
        uint16_t f_num = slot->channel->f_num;
        int8_t range = (f_num >> 7) & 7;
        uint8_t vibpos = chip->vibpos;
        if (!(vibpos & 3)) range = 0;
        else if (vibpos & 1) range >>= 1;
        range >>= chip->vibshift;
        if (vibpos & 4) range = (int8_t)-range;
        f_num = (uint16_t)(f_num + range);
        inc = ((((uint32_t)f_num << slot->channel->block) >> 1) * mt[slot->reg_mult]) >> 1;
    } else {
        inc = slot->pg_inc;
    }
    phase = (uint16_t)(slot->pg_phase >> 9);
    if (slot->pg_reset) slot->pg_phase = 0;
    slot->pg_phase += inc;
    noise = chip->noise;
    slot->pg_phase_out = phase;
    if (slot->slot_num == 13) {                                       /* hh */
        chip->rm_hh_bit2 = (phase >> 2) & 1; chip->rm_hh_bit3 = (phase >> 3) & 1;
        chip->rm_hh_bit7 = (phase >> 7) & 1; chip->rm_hh_bit8 = (phase >> 8) & 1;
    }
    if (slot->slot_num == 17 && (chip->rhy & 0x20)) {                /* tc */
        chip->rm_tc_bit3 = (phase >> 3) & 1; chip->rm_tc_bit5 = (phase >> 5) & 1;
    }
    if ((chip->rhy & 0x20) && slot->slot_num >= 13) {
        uint8_t rm_xor = (chip->rm_hh_bit2 ^ chip->rm_hh_bit7) | (chip->rm_hh_bit3 ^ chip->rm_tc_bit5) | (chip->rm_tc_bit3 ^ chip->rm_tc_bit5);
        switch (slot->slot_num) {
        case 13: slot->pg_phase_out = (uint16_t)(rm_xor << 9); slot->pg_phase_out |= (rm_xor ^ (noise & 1)) ? 0xd0 : 0x34; break;
        case 16: slot->pg_phase_out = (uint16_t)((chip->rm_hh_bit8 << 9) | ((chip->rm_hh_bit8 ^ (noise & 1)) << 8)); break;
        case 17: slot->pg_phase_out = (uint16_t)((rm_xor << 9) | 0x80); break;
        default: break;
        }
    }
    chip->noise = (noise >> 1) | ((((noise >> 14) ^ noise) & 0x01) << 22);

    /* output */
    {
        uint16_t ph = (uint16_t)(slot->pg_phase_out + *slot->mod);
        switch (slot->reg_wf) {
        case 0: slot->out = OPL3_EnvelopeCalcSin0(ph, slot->eg_out); break;
        case 1: slot->out = OPL3_EnvelopeCalcSin1(ph, slot->eg_out); break;
        case 2: slot->out = OPL3_EnvelopeCalcSin2(ph, slot->eg_out); break;
        default: slot->out = OPL3_EnvelopeCalcSin3(ph, slot->eg_out); break;
        }
    }
}

/* OPL2 fast path. Nuked-OPL3 (OPL3_Generate) processes all 36 operator slots and 18 channels every sample;
 * with the OPL3 mode off and bank 1 never written, slots 18..35 stay silent in their reset state, so this
 * does the same work for the 18 slots and 9 channels that exist, mono. Same tables, same functions. */
static int16_t opl2_generate(opl3_chip *chip)
{
    opl3_writebuf *writebuf;
    int32_t mix = 0;
    uint32_t ii;
    uint8_t shift = 0;
    /* The noise generator is clocked once per slot processed, 36 times a sample, and the hi-hat, snare and
     * cymbal read it at their turn: a skipped slot still clocks it, and so do the 18 slots of bank 1. */
    for (ii = 0; ii < 12u; ii++) {           /* melodic slots: a dormant one (key off, envelope at the floor, output
                                                 history zero) outputs exactly 0 and changes only on a key-on */
        opl3_slot *s = &chip->slot[ii];
        if (OPL_NO_SKIP || s->key || s->eg_gen != envelope_gen_num_release || s->eg_rout != 0x1ff) {
            opl2_slot(s);
        } else {                                /* dormant: the envelope stays put and the level is 0, but the phase
                                                   runs on and a sine slot gives the chip's "negative zero", -1,
                                                   on its negative half cycle, which feeds back and into the mix */
            if (s->channel->fb) s->fbmod = (s->prout + s->out) >> (0x09 - s->channel->fb); else s->fbmod = 0;
            s->prout = s->out;
            if (s->reg_vib) {
                OPL3_PhaseGenerate(s);          /* vibrato: Nuked's own, it clocks the noise too */
            } else {
                s->pg_phase_out = (uint16_t)(s->pg_phase >> 9);
                s->pg_phase += s->pg_inc;
                NOISE_STEP(chip);
            }
            s->out = (s->reg_wf == 0 && ((uint16_t)(s->pg_phase_out + *s->mod) & 0x200u)) ? -1 : 0;
        }
    }
    for (ii = 12; ii < 18u; ii++)             /* the rhythm slots: HH, SD and TC borrow phases from 13 and 17 */
        opl2_slot(&chip->slot[ii]);
    NOISE_JUMP18(chip);                       /* bank 1: silent, but its 18 slots clock the noise */
    for (ii = 0; ii < 9u; ii++) {             /* (channels 7 and 8 in rhythm mode weight their second slot x3 through out[1..3]) */
        opl3_channel *channel = &chip->channel[ii];
        int16_t **out = channel->out;
        int16_t accm = (int16_t)(*out[0] + *out[1] + *out[2] + *out[3]);
        mix += (int16_t)(accm & channel->cha);
    }
    if ((chip->timer & 0x3f) == 0x3f)
        chip->tremolopos = (chip->tremolopos + 1) % 210;
    chip->tremolo = (uint8_t)((chip->tremolopos < 105 ? chip->tremolopos : 210 - chip->tremolopos) >> chip->tremoloshift);
    if ((chip->timer & 0x3ff) == 0x3ff)
        chip->vibpos = (chip->vibpos + 1) & 7;
    chip->timer++;
    if (chip->eg_state) {
        uint32_t lo = (uint32_t)chip->eg_timer;   /* the scan looks at bits 0..12 only */
        while (shift < 13 && ((lo >> shift) & 1) == 0)
            shift++;
        chip->eg_add = shift > 12 ? 0 : (uint8_t)(shift + 1);
        chip->eg_timer_lo = (uint8_t)(chip->eg_timer & 0x3u);
    }
    if (chip->eg_timerrem || chip->eg_state) {
        if (chip->eg_timer == UINT64_C(0xfffffffff)) { chip->eg_timer = 0; chip->eg_timerrem = 1; }
        else { chip->eg_timer++; chip->eg_timerrem = 0; }
    }
    chip->eg_state ^= 1;
    while ((writebuf = &chip->writebuf[chip->writebuf_cur]), writebuf->time <= chip->writebuf_samplecnt) {
        if (!(writebuf->reg & 0x200))
            break;
        writebuf->reg &= 0x1ff;
        OPL3_WriteReg(chip, writebuf->reg, writebuf->data);
        chip->writebuf_cur = (chip->writebuf_cur + 1) & (OPL_WRITEBUF_SIZE - 1);
    }
    chip->writebuf_samplecnt++;
    return OPL3_ClipSample(mix);
}

