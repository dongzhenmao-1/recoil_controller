#include <nlohmann/json.hpp>

namespace mtd {
    template <typename tnt>
    struct point2 { 
        tnt x, y; 
        point2() = default;
        template <typename tnt> point2(tnt _x, tnt _y) : x(_x), y(_y) {}
        template <typename ont> point2(const point2<ont>& other) : x(tnt(other.x)), y(tnt(other.y)) {}
    };
    
    template <typename ant, typename bnt> bool operator <(const point2<ant> &a, const point2<bnt> &b) {
        return a.x == b.x ? a.y < b.y : a.x < b.x;
    }
    template <typename ant, typename bnt> point2<ant> operator +(const point2<ant> &a, const point2<bnt> &b) {
        return {a.x + b.x, a.y + b.y};
    }
    template <typename ant, typename bnt> point2<ant> operator -(const point2<ant> &a, const point2<bnt> &b) {
        return {a.x - b.x, a.y - b.y};
    }
    template <typename ant, typename bnt> point2<ant> operator -(const point2<ant> &a) {
        return {-a.x, -a.y};
    }
    template <typename ant, typename bnt> point2<ant> operator *(const point2<ant> &a, const bnt &b) {
        return {a.x * b, a.y * b};
    }
    template <typename ant, typename bnt> point2<ant> operator *(const ant &a, const point2<bnt> &b) {
        return {a * b.x, a * b.y};
    }
    template <typename ant, typename bnt> point2<ant> operator /(const point2<ant> &a, const bnt &b) {
        return {a.x / b, a.y / b};
    }

    using point2d = point2<double>;
    using point2i = point2<int>;

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(point2d, x, y)
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(point2i, x, y)

}