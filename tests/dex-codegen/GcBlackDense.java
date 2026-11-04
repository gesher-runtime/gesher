/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher;

import java.util.ArrayList;
import java.util.List;

/**
 * Stop-the-world compaction of the CMC collector (Windows has no userfaultfd, so ART
 * falls back to it, mark_compact.cc) over classes whose instance-reference bitmap
 * spans pages (patches/art/0760-cmc-fallback-black-dense-page.patch). An instance's
 * references are found through its class's overflow bitmap, at the end of the class
 * object; a zero bitmap word on a page of its own made the collector move that page
 * of the class with UFFDIO_COPY, which aborted the runtime ("ioctl_userfaultfd: copy
 * failed: Bad file descriptor", Waze during startup).
 *
 * Each W* class object is over 4 KiB (Wide's 600 virtual methods are in its
 * embedded vtable) and ends with a bitmap of 65 words, all zero but the last
 * (Wide's 1024 long fields come before W*'s two references), so some of the
 * 64 classes have such a page wherever they are allocated. Their instances are kept
 * live in a dense region of the heap while garbage is allocated and collected, then
 * every reference and long field is checked.
 *
 * Prints "GcBlackDense all passed".
 */
public final class GcBlackDense {
    abstract static class Wide {
        long f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15;
        long f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31;
        long f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45, f46, f47;
        long f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61, f62, f63;
        long f64, f65, f66, f67, f68, f69, f70, f71, f72, f73, f74, f75, f76, f77, f78, f79;
        long f80, f81, f82, f83, f84, f85, f86, f87, f88, f89, f90, f91, f92, f93, f94, f95;
        long f96, f97, f98, f99, f100, f101, f102, f103, f104, f105, f106, f107, f108, f109, f110, f111;
        long f112, f113, f114, f115, f116, f117, f118, f119, f120, f121, f122, f123, f124, f125, f126, f127;
        long f128, f129, f130, f131, f132, f133, f134, f135, f136, f137, f138, f139, f140, f141, f142, f143;
        long f144, f145, f146, f147, f148, f149, f150, f151, f152, f153, f154, f155, f156, f157, f158, f159;
        long f160, f161, f162, f163, f164, f165, f166, f167, f168, f169, f170, f171, f172, f173, f174, f175;
        long f176, f177, f178, f179, f180, f181, f182, f183, f184, f185, f186, f187, f188, f189, f190, f191;
        long f192, f193, f194, f195, f196, f197, f198, f199, f200, f201, f202, f203, f204, f205, f206, f207;
        long f208, f209, f210, f211, f212, f213, f214, f215, f216, f217, f218, f219, f220, f221, f222, f223;
        long f224, f225, f226, f227, f228, f229, f230, f231, f232, f233, f234, f235, f236, f237, f238, f239;
        long f240, f241, f242, f243, f244, f245, f246, f247, f248, f249, f250, f251, f252, f253, f254, f255;
        long f256, f257, f258, f259, f260, f261, f262, f263, f264, f265, f266, f267, f268, f269, f270, f271;
        long f272, f273, f274, f275, f276, f277, f278, f279, f280, f281, f282, f283, f284, f285, f286, f287;
        long f288, f289, f290, f291, f292, f293, f294, f295, f296, f297, f298, f299, f300, f301, f302, f303;
        long f304, f305, f306, f307, f308, f309, f310, f311, f312, f313, f314, f315, f316, f317, f318, f319;
        long f320, f321, f322, f323, f324, f325, f326, f327, f328, f329, f330, f331, f332, f333, f334, f335;
        long f336, f337, f338, f339, f340, f341, f342, f343, f344, f345, f346, f347, f348, f349, f350, f351;
        long f352, f353, f354, f355, f356, f357, f358, f359, f360, f361, f362, f363, f364, f365, f366, f367;
        long f368, f369, f370, f371, f372, f373, f374, f375, f376, f377, f378, f379, f380, f381, f382, f383;
        long f384, f385, f386, f387, f388, f389, f390, f391, f392, f393, f394, f395, f396, f397, f398, f399;
        long f400, f401, f402, f403, f404, f405, f406, f407, f408, f409, f410, f411, f412, f413, f414, f415;
        long f416, f417, f418, f419, f420, f421, f422, f423, f424, f425, f426, f427, f428, f429, f430, f431;
        long f432, f433, f434, f435, f436, f437, f438, f439, f440, f441, f442, f443, f444, f445, f446, f447;
        long f448, f449, f450, f451, f452, f453, f454, f455, f456, f457, f458, f459, f460, f461, f462, f463;
        long f464, f465, f466, f467, f468, f469, f470, f471, f472, f473, f474, f475, f476, f477, f478, f479;
        long f480, f481, f482, f483, f484, f485, f486, f487, f488, f489, f490, f491, f492, f493, f494, f495;
        long f496, f497, f498, f499, f500, f501, f502, f503, f504, f505, f506, f507, f508, f509, f510, f511;
        long f512, f513, f514, f515, f516, f517, f518, f519, f520, f521, f522, f523, f524, f525, f526, f527;
        long f528, f529, f530, f531, f532, f533, f534, f535, f536, f537, f538, f539, f540, f541, f542, f543;
        long f544, f545, f546, f547, f548, f549, f550, f551, f552, f553, f554, f555, f556, f557, f558, f559;
        long f560, f561, f562, f563, f564, f565, f566, f567, f568, f569, f570, f571, f572, f573, f574, f575;
        long f576, f577, f578, f579, f580, f581, f582, f583, f584, f585, f586, f587, f588, f589, f590, f591;
        long f592, f593, f594, f595, f596, f597, f598, f599, f600, f601, f602, f603, f604, f605, f606, f607;
        long f608, f609, f610, f611, f612, f613, f614, f615, f616, f617, f618, f619, f620, f621, f622, f623;
        long f624, f625, f626, f627, f628, f629, f630, f631, f632, f633, f634, f635, f636, f637, f638, f639;
        long f640, f641, f642, f643, f644, f645, f646, f647, f648, f649, f650, f651, f652, f653, f654, f655;
        long f656, f657, f658, f659, f660, f661, f662, f663, f664, f665, f666, f667, f668, f669, f670, f671;
        long f672, f673, f674, f675, f676, f677, f678, f679, f680, f681, f682, f683, f684, f685, f686, f687;
        long f688, f689, f690, f691, f692, f693, f694, f695, f696, f697, f698, f699, f700, f701, f702, f703;
        long f704, f705, f706, f707, f708, f709, f710, f711, f712, f713, f714, f715, f716, f717, f718, f719;
        long f720, f721, f722, f723, f724, f725, f726, f727, f728, f729, f730, f731, f732, f733, f734, f735;
        long f736, f737, f738, f739, f740, f741, f742, f743, f744, f745, f746, f747, f748, f749, f750, f751;
        long f752, f753, f754, f755, f756, f757, f758, f759, f760, f761, f762, f763, f764, f765, f766, f767;
        long f768, f769, f770, f771, f772, f773, f774, f775, f776, f777, f778, f779, f780, f781, f782, f783;
        long f784, f785, f786, f787, f788, f789, f790, f791, f792, f793, f794, f795, f796, f797, f798, f799;
        long f800, f801, f802, f803, f804, f805, f806, f807, f808, f809, f810, f811, f812, f813, f814, f815;
        long f816, f817, f818, f819, f820, f821, f822, f823, f824, f825, f826, f827, f828, f829, f830, f831;
        long f832, f833, f834, f835, f836, f837, f838, f839, f840, f841, f842, f843, f844, f845, f846, f847;
        long f848, f849, f850, f851, f852, f853, f854, f855, f856, f857, f858, f859, f860, f861, f862, f863;
        long f864, f865, f866, f867, f868, f869, f870, f871, f872, f873, f874, f875, f876, f877, f878, f879;
        long f880, f881, f882, f883, f884, f885, f886, f887, f888, f889, f890, f891, f892, f893, f894, f895;
        long f896, f897, f898, f899, f900, f901, f902, f903, f904, f905, f906, f907, f908, f909, f910, f911;
        long f912, f913, f914, f915, f916, f917, f918, f919, f920, f921, f922, f923, f924, f925, f926, f927;
        long f928, f929, f930, f931, f932, f933, f934, f935, f936, f937, f938, f939, f940, f941, f942, f943;
        long f944, f945, f946, f947, f948, f949, f950, f951, f952, f953, f954, f955, f956, f957, f958, f959;
        long f960, f961, f962, f963, f964, f965, f966, f967, f968, f969, f970, f971, f972, f973, f974, f975;
        long f976, f977, f978, f979, f980, f981, f982, f983, f984, f985, f986, f987, f988, f989, f990, f991;
        long f992, f993, f994, f995, f996, f997, f998, f999, f1000, f1001, f1002, f1003, f1004, f1005, f1006, f1007;
        long f1008, f1009, f1010, f1011, f1012, f1013, f1014, f1015, f1016, f1017, f1018, f1019, f1020, f1021, f1022, f1023;

        abstract void set(Object a, Object b);
        abstract Object a();
        abstract Object b();

        int m0() { return 0; } int m1() { return 1; } int m2() { return 2; } int m3() { return 3; } int m4() { return 4; } int m5() { return 5; }
        int m6() { return 6; } int m7() { return 7; } int m8() { return 8; } int m9() { return 9; } int m10() { return 10; } int m11() { return 11; }
        int m12() { return 12; } int m13() { return 13; } int m14() { return 14; } int m15() { return 15; } int m16() { return 16; } int m17() { return 17; }
        int m18() { return 18; } int m19() { return 19; } int m20() { return 20; } int m21() { return 21; } int m22() { return 22; } int m23() { return 23; }
        int m24() { return 24; } int m25() { return 25; } int m26() { return 26; } int m27() { return 27; } int m28() { return 28; } int m29() { return 29; }
        int m30() { return 30; } int m31() { return 31; } int m32() { return 32; } int m33() { return 33; } int m34() { return 34; } int m35() { return 35; }
        int m36() { return 36; } int m37() { return 37; } int m38() { return 38; } int m39() { return 39; } int m40() { return 40; } int m41() { return 41; }
        int m42() { return 42; } int m43() { return 43; } int m44() { return 44; } int m45() { return 45; } int m46() { return 46; } int m47() { return 47; }
        int m48() { return 48; } int m49() { return 49; } int m50() { return 50; } int m51() { return 51; } int m52() { return 52; } int m53() { return 53; }
        int m54() { return 54; } int m55() { return 55; } int m56() { return 56; } int m57() { return 57; } int m58() { return 58; } int m59() { return 59; }
        int m60() { return 60; } int m61() { return 61; } int m62() { return 62; } int m63() { return 63; } int m64() { return 64; } int m65() { return 65; }
        int m66() { return 66; } int m67() { return 67; } int m68() { return 68; } int m69() { return 69; } int m70() { return 70; } int m71() { return 71; }
        int m72() { return 72; } int m73() { return 73; } int m74() { return 74; } int m75() { return 75; } int m76() { return 76; } int m77() { return 77; }
        int m78() { return 78; } int m79() { return 79; } int m80() { return 80; } int m81() { return 81; } int m82() { return 82; } int m83() { return 83; }
        int m84() { return 84; } int m85() { return 85; } int m86() { return 86; } int m87() { return 87; } int m88() { return 88; } int m89() { return 89; }
        int m90() { return 90; } int m91() { return 91; } int m92() { return 92; } int m93() { return 93; } int m94() { return 94; } int m95() { return 95; }
        int m96() { return 96; } int m97() { return 97; } int m98() { return 98; } int m99() { return 99; } int m100() { return 100; } int m101() { return 101; }
        int m102() { return 102; } int m103() { return 103; } int m104() { return 104; } int m105() { return 105; } int m106() { return 106; } int m107() { return 107; }
        int m108() { return 108; } int m109() { return 109; } int m110() { return 110; } int m111() { return 111; } int m112() { return 112; } int m113() { return 113; }
        int m114() { return 114; } int m115() { return 115; } int m116() { return 116; } int m117() { return 117; } int m118() { return 118; } int m119() { return 119; }
        int m120() { return 120; } int m121() { return 121; } int m122() { return 122; } int m123() { return 123; } int m124() { return 124; } int m125() { return 125; }
        int m126() { return 126; } int m127() { return 127; } int m128() { return 128; } int m129() { return 129; } int m130() { return 130; } int m131() { return 131; }
        int m132() { return 132; } int m133() { return 133; } int m134() { return 134; } int m135() { return 135; } int m136() { return 136; } int m137() { return 137; }
        int m138() { return 138; } int m139() { return 139; } int m140() { return 140; } int m141() { return 141; } int m142() { return 142; } int m143() { return 143; }
        int m144() { return 144; } int m145() { return 145; } int m146() { return 146; } int m147() { return 147; } int m148() { return 148; } int m149() { return 149; }
        int m150() { return 150; } int m151() { return 151; } int m152() { return 152; } int m153() { return 153; } int m154() { return 154; } int m155() { return 155; }
        int m156() { return 156; } int m157() { return 157; } int m158() { return 158; } int m159() { return 159; } int m160() { return 160; } int m161() { return 161; }
        int m162() { return 162; } int m163() { return 163; } int m164() { return 164; } int m165() { return 165; } int m166() { return 166; } int m167() { return 167; }
        int m168() { return 168; } int m169() { return 169; } int m170() { return 170; } int m171() { return 171; } int m172() { return 172; } int m173() { return 173; }
        int m174() { return 174; } int m175() { return 175; } int m176() { return 176; } int m177() { return 177; } int m178() { return 178; } int m179() { return 179; }
        int m180() { return 180; } int m181() { return 181; } int m182() { return 182; } int m183() { return 183; } int m184() { return 184; } int m185() { return 185; }
        int m186() { return 186; } int m187() { return 187; } int m188() { return 188; } int m189() { return 189; } int m190() { return 190; } int m191() { return 191; }
        int m192() { return 192; } int m193() { return 193; } int m194() { return 194; } int m195() { return 195; } int m196() { return 196; } int m197() { return 197; }
        int m198() { return 198; } int m199() { return 199; } int m200() { return 200; } int m201() { return 201; } int m202() { return 202; } int m203() { return 203; }
        int m204() { return 204; } int m205() { return 205; } int m206() { return 206; } int m207() { return 207; } int m208() { return 208; } int m209() { return 209; }
        int m210() { return 210; } int m211() { return 211; } int m212() { return 212; } int m213() { return 213; } int m214() { return 214; } int m215() { return 215; }
        int m216() { return 216; } int m217() { return 217; } int m218() { return 218; } int m219() { return 219; } int m220() { return 220; } int m221() { return 221; }
        int m222() { return 222; } int m223() { return 223; } int m224() { return 224; } int m225() { return 225; } int m226() { return 226; } int m227() { return 227; }
        int m228() { return 228; } int m229() { return 229; } int m230() { return 230; } int m231() { return 231; } int m232() { return 232; } int m233() { return 233; }
        int m234() { return 234; } int m235() { return 235; } int m236() { return 236; } int m237() { return 237; } int m238() { return 238; } int m239() { return 239; }
        int m240() { return 240; } int m241() { return 241; } int m242() { return 242; } int m243() { return 243; } int m244() { return 244; } int m245() { return 245; }
        int m246() { return 246; } int m247() { return 247; } int m248() { return 248; } int m249() { return 249; } int m250() { return 250; } int m251() { return 251; }
        int m252() { return 252; } int m253() { return 253; } int m254() { return 254; } int m255() { return 255; } int m256() { return 256; } int m257() { return 257; }
        int m258() { return 258; } int m259() { return 259; } int m260() { return 260; } int m261() { return 261; } int m262() { return 262; } int m263() { return 263; }
        int m264() { return 264; } int m265() { return 265; } int m266() { return 266; } int m267() { return 267; } int m268() { return 268; } int m269() { return 269; }
        int m270() { return 270; } int m271() { return 271; } int m272() { return 272; } int m273() { return 273; } int m274() { return 274; } int m275() { return 275; }
        int m276() { return 276; } int m277() { return 277; } int m278() { return 278; } int m279() { return 279; } int m280() { return 280; } int m281() { return 281; }
        int m282() { return 282; } int m283() { return 283; } int m284() { return 284; } int m285() { return 285; } int m286() { return 286; } int m287() { return 287; }
        int m288() { return 288; } int m289() { return 289; } int m290() { return 290; } int m291() { return 291; } int m292() { return 292; } int m293() { return 293; }
        int m294() { return 294; } int m295() { return 295; } int m296() { return 296; } int m297() { return 297; } int m298() { return 298; } int m299() { return 299; }
        int m300() { return 300; } int m301() { return 301; } int m302() { return 302; } int m303() { return 303; } int m304() { return 304; } int m305() { return 305; }
        int m306() { return 306; } int m307() { return 307; } int m308() { return 308; } int m309() { return 309; } int m310() { return 310; } int m311() { return 311; }
        int m312() { return 312; } int m313() { return 313; } int m314() { return 314; } int m315() { return 315; } int m316() { return 316; } int m317() { return 317; }
        int m318() { return 318; } int m319() { return 319; } int m320() { return 320; } int m321() { return 321; } int m322() { return 322; } int m323() { return 323; }
        int m324() { return 324; } int m325() { return 325; } int m326() { return 326; } int m327() { return 327; } int m328() { return 328; } int m329() { return 329; }
        int m330() { return 330; } int m331() { return 331; } int m332() { return 332; } int m333() { return 333; } int m334() { return 334; } int m335() { return 335; }
        int m336() { return 336; } int m337() { return 337; } int m338() { return 338; } int m339() { return 339; } int m340() { return 340; } int m341() { return 341; }
        int m342() { return 342; } int m343() { return 343; } int m344() { return 344; } int m345() { return 345; } int m346() { return 346; } int m347() { return 347; }
        int m348() { return 348; } int m349() { return 349; } int m350() { return 350; } int m351() { return 351; } int m352() { return 352; } int m353() { return 353; }
        int m354() { return 354; } int m355() { return 355; } int m356() { return 356; } int m357() { return 357; } int m358() { return 358; } int m359() { return 359; }
        int m360() { return 360; } int m361() { return 361; } int m362() { return 362; } int m363() { return 363; } int m364() { return 364; } int m365() { return 365; }
        int m366() { return 366; } int m367() { return 367; } int m368() { return 368; } int m369() { return 369; } int m370() { return 370; } int m371() { return 371; }
        int m372() { return 372; } int m373() { return 373; } int m374() { return 374; } int m375() { return 375; } int m376() { return 376; } int m377() { return 377; }
        int m378() { return 378; } int m379() { return 379; } int m380() { return 380; } int m381() { return 381; } int m382() { return 382; } int m383() { return 383; }
        int m384() { return 384; } int m385() { return 385; } int m386() { return 386; } int m387() { return 387; } int m388() { return 388; } int m389() { return 389; }
        int m390() { return 390; } int m391() { return 391; } int m392() { return 392; } int m393() { return 393; } int m394() { return 394; } int m395() { return 395; }
        int m396() { return 396; } int m397() { return 397; } int m398() { return 398; } int m399() { return 399; } int m400() { return 400; } int m401() { return 401; }
        int m402() { return 402; } int m403() { return 403; } int m404() { return 404; } int m405() { return 405; } int m406() { return 406; } int m407() { return 407; }
        int m408() { return 408; } int m409() { return 409; } int m410() { return 410; } int m411() { return 411; } int m412() { return 412; } int m413() { return 413; }
        int m414() { return 414; } int m415() { return 415; } int m416() { return 416; } int m417() { return 417; } int m418() { return 418; } int m419() { return 419; }
        int m420() { return 420; } int m421() { return 421; } int m422() { return 422; } int m423() { return 423; } int m424() { return 424; } int m425() { return 425; }
        int m426() { return 426; } int m427() { return 427; } int m428() { return 428; } int m429() { return 429; } int m430() { return 430; } int m431() { return 431; }
        int m432() { return 432; } int m433() { return 433; } int m434() { return 434; } int m435() { return 435; } int m436() { return 436; } int m437() { return 437; }
        int m438() { return 438; } int m439() { return 439; } int m440() { return 440; } int m441() { return 441; } int m442() { return 442; } int m443() { return 443; }
        int m444() { return 444; } int m445() { return 445; } int m446() { return 446; } int m447() { return 447; } int m448() { return 448; } int m449() { return 449; }
        int m450() { return 450; } int m451() { return 451; } int m452() { return 452; } int m453() { return 453; } int m454() { return 454; } int m455() { return 455; }
        int m456() { return 456; } int m457() { return 457; } int m458() { return 458; } int m459() { return 459; } int m460() { return 460; } int m461() { return 461; }
        int m462() { return 462; } int m463() { return 463; } int m464() { return 464; } int m465() { return 465; } int m466() { return 466; } int m467() { return 467; }
        int m468() { return 468; } int m469() { return 469; } int m470() { return 470; } int m471() { return 471; } int m472() { return 472; } int m473() { return 473; }
        int m474() { return 474; } int m475() { return 475; } int m476() { return 476; } int m477() { return 477; } int m478() { return 478; } int m479() { return 479; }
        int m480() { return 480; } int m481() { return 481; } int m482() { return 482; } int m483() { return 483; } int m484() { return 484; } int m485() { return 485; }
        int m486() { return 486; } int m487() { return 487; } int m488() { return 488; } int m489() { return 489; } int m490() { return 490; } int m491() { return 491; }
        int m492() { return 492; } int m493() { return 493; } int m494() { return 494; } int m495() { return 495; } int m496() { return 496; } int m497() { return 497; }
        int m498() { return 498; } int m499() { return 499; } int m500() { return 500; } int m501() { return 501; } int m502() { return 502; } int m503() { return 503; }
        int m504() { return 504; } int m505() { return 505; } int m506() { return 506; } int m507() { return 507; } int m508() { return 508; } int m509() { return 509; }
        int m510() { return 510; } int m511() { return 511; } int m512() { return 512; } int m513() { return 513; } int m514() { return 514; } int m515() { return 515; }
        int m516() { return 516; } int m517() { return 517; } int m518() { return 518; } int m519() { return 519; } int m520() { return 520; } int m521() { return 521; }
        int m522() { return 522; } int m523() { return 523; } int m524() { return 524; } int m525() { return 525; } int m526() { return 526; } int m527() { return 527; }
        int m528() { return 528; } int m529() { return 529; } int m530() { return 530; } int m531() { return 531; } int m532() { return 532; } int m533() { return 533; }
        int m534() { return 534; } int m535() { return 535; } int m536() { return 536; } int m537() { return 537; } int m538() { return 538; } int m539() { return 539; }
        int m540() { return 540; } int m541() { return 541; } int m542() { return 542; } int m543() { return 543; } int m544() { return 544; } int m545() { return 545; }
        int m546() { return 546; } int m547() { return 547; } int m548() { return 548; } int m549() { return 549; } int m550() { return 550; } int m551() { return 551; }
        int m552() { return 552; } int m553() { return 553; } int m554() { return 554; } int m555() { return 555; } int m556() { return 556; } int m557() { return 557; }
        int m558() { return 558; } int m559() { return 559; } int m560() { return 560; } int m561() { return 561; } int m562() { return 562; } int m563() { return 563; }
        int m564() { return 564; } int m565() { return 565; } int m566() { return 566; } int m567() { return 567; } int m568() { return 568; } int m569() { return 569; }
        int m570() { return 570; } int m571() { return 571; } int m572() { return 572; } int m573() { return 573; } int m574() { return 574; } int m575() { return 575; }
        int m576() { return 576; } int m577() { return 577; } int m578() { return 578; } int m579() { return 579; } int m580() { return 580; } int m581() { return 581; }
        int m582() { return 582; } int m583() { return 583; } int m584() { return 584; } int m585() { return 585; } int m586() { return 586; } int m587() { return 587; }
        int m588() { return 588; } int m589() { return 589; } int m590() { return 590; } int m591() { return 591; } int m592() { return 592; } int m593() { return 593; }
        int m594() { return 594; } int m595() { return 595; } int m596() { return 596; } int m597() { return 597; } int m598() { return 598; } int m599() { return 599; }
    }

    static final class W0 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W1 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W2 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W3 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W4 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W5 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W6 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W7 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W8 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W9 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W10 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W11 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W12 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W13 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W14 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W15 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W16 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W17 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W18 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W19 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W20 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W21 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W22 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W23 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W24 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W25 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W26 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W27 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W28 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W29 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W30 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W31 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W32 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W33 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W34 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W35 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W36 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W37 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W38 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W39 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W40 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W41 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W42 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W43 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W44 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W45 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W46 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W47 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W48 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W49 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W50 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W51 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W52 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W53 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W54 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W55 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W56 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W57 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W58 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W59 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W60 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W61 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W62 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }
    static final class W63 extends Wide { Object a, b; void set(Object x, Object y) { a = x; b = y; } Object a() { return a; } Object b() { return b; } }

    static final Class<?>[] TYPES = {
        W0.class, W1.class, W2.class, W3.class, W4.class, W5.class, W6.class, W7.class,
        W8.class, W9.class, W10.class, W11.class, W12.class, W13.class, W14.class, W15.class,
        W16.class, W17.class, W18.class, W19.class, W20.class, W21.class, W22.class, W23.class,
        W24.class, W25.class, W26.class, W27.class, W28.class, W29.class, W30.class, W31.class,
        W32.class, W33.class, W34.class, W35.class, W36.class, W37.class, W38.class, W39.class,
        W40.class, W41.class, W42.class, W43.class, W44.class, W45.class, W46.class, W47.class,
        W48.class, W49.class, W50.class, W51.class, W52.class, W53.class, W54.class, W55.class,
        W56.class, W57.class, W58.class, W59.class, W60.class, W61.class, W62.class, W63.class,
    };

    static void fill(Wide w, long seed) {
        w.f0 = seed;
        w.f512 = ~seed;
        w.f1023 = seed * 31;
    }

    static boolean intact(Wide w, long seed, List<String> names) {
        return w.f0 == seed && w.f512 == ~seed && w.f1023 == seed * 31
                && names.get((int) seed).equals(w.a()) && w.b() instanceof Wide
                && w.m599() == 599;
    }

    public static void main(String[] args) throws Exception {
        int rounds = args.length > 0 ? Integer.parseInt(args[0]) : 30;
        List<Wide> live = new ArrayList<>();
        List<String> names = new ArrayList<>();
        List<Object> garbage = new ArrayList<>();
        for (int copy = 0; copy < 8; copy++) {
            for (Class<?> type : TYPES) {
                Wide w = (Wide) type.getDeclaredConstructor().newInstance();
                long seed = live.size();
                names.add(type.getSimpleName() + "#" + seed);
                fill(w, seed);
                live.add(w);
                garbage.add(new byte[1024 + (int) (seed % 7) * 512]);
            }
        }
        for (int i = 0; i < live.size(); i++) {
            Wide w = live.get(i);
            w.set(names.get(i), live.get((i * 7 + 1) % live.size()));
        }
        // Collections the heap starts for allocation, not Runtime.gc(): an explicit
        // collection compacts everything, and only the others keep a dense region (the
        // old generation of a young collection) in place.
        java.lang.reflect.Method stat = Class.forName("dalvik.system.VMDebug")
                .getMethod("getRuntimeStat", String.class);
        int start = Integer.parseInt((String) stat.invoke(null, "art.gc.gc-count"));
        int gcs = 0;
        long deadline = System.nanoTime() + 120_000_000_000L;
        while (gcs < rounds && System.nanoTime() < deadline) {
            for (int i = 0; i < 20000; i++) {
                garbage.add(new Object[] {new byte[64 + i % 400], "g" + i});
                if (garbage.size() > 3000) garbage.clear();
            }
            int now = Integer.parseInt((String) stat.invoke(null, "art.gc.gc-count")) - start;
            if (now == gcs) continue;
            gcs = now;
            for (int i = 0; i < live.size(); i++) {
                if (!intact(live.get(i), i, names)) {
                    System.out.println("FAIL GcBlackDense: " + names.get(i) + " changed after GC " + gcs);
                    System.exit(1);
                }
            }
        }
        if (gcs < rounds) {
            System.out.println("FAIL GcBlackDense: only " + gcs + " GCs in 120 s");
            System.exit(1);
        }
        System.out.println("ok   GcBlackDense: " + live.size() + " instances of " + TYPES.length
                + " classes with page-spanning reference bitmaps intact after " + gcs + " GCs");
        System.out.println("GcBlackDense all passed");
    }
}
