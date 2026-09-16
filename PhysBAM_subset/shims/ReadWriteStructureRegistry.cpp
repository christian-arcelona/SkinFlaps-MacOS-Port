// Minimal registry storage definitions for STRUCTURE read/write templates.
// Headless parity paths do not use PhysBAM file I/O registration, but some
// instantiated geometry code requires these symbols at link time.

#ifndef COMPILE_WITHOUT_READ_WRITE_SUPPORT

#include <PhysBAM_Tools/Vectors/VECTOR.h>
#include <PhysBAM_Geometry/Read_Write/Geometry/READ_WRITE_STRUCTURE.h>

namespace PhysBAM {

template<class RW, class TV>
HASHTABLE<std::string, void (*)(std::istream&, STRUCTURE<TV>&)>&
Read_Write<STRUCTURE<TV>, RW>::Read_Registry()
{
    static HASHTABLE<std::string, void (*)(std::istream&, STRUCTURE<TV>&)> registry;
    return registry;
}

template<class RW, class TV>
HASHTABLE<std::string, void (*)(std::istream&, STRUCTURE<TV>&)>&
Read_Write<STRUCTURE<TV>, RW>::Read_Structure_Registry()
{
    static HASHTABLE<std::string, void (*)(std::istream&, STRUCTURE<TV>&)> registry;
    return registry;
}

template<class RW, class TV>
HASHTABLE<std::string, void (*)(std::ostream&, const STRUCTURE<TV>&)>&
Read_Write<STRUCTURE<TV>, RW>::Write_Registry()
{
    static HASHTABLE<std::string, void (*)(std::ostream&, const STRUCTURE<TV>&)> registry;
    return registry;
}

template<class RW, class TV>
HASHTABLE<std::string, void (*)(std::ostream&, const STRUCTURE<TV>&)>&
Read_Write<STRUCTURE<TV>, RW>::Write_Structure_Registry()
{
    static HASHTABLE<std::string, void (*)(std::ostream&, const STRUCTURE<TV>&)> registry;
    return registry;
}

#define INSTANTIATE_RW(T, RW) \
    template class Read_Write<STRUCTURE<VECTOR<T, 1>>, RW>; \
    template class Read_Write<STRUCTURE<VECTOR<T, 2>>, RW>; \
    template class Read_Write<STRUCTURE<VECTOR<T, 3>>, RW>;

INSTANTIATE_RW(float, float)
INSTANTIATE_RW(float, double)
#ifndef COMPILE_WITHOUT_DOUBLE_SUPPORT
INSTANTIATE_RW(double, float)
INSTANTIATE_RW(double, double)
#endif

#undef INSTANTIATE_RW

} // namespace PhysBAM

#endif
