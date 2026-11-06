// SpectroCraft - Model session: data model, expression parser, JSON I/O and
// time-reference warping for parametric sound-source models.
#include "JuceHeader.h"
#include "ModelSession.h"
#include "FFTHelpers.h"   // scfft::realForward

namespace sc::model
{
    // ============================================================ remap curve

    double RemapCurve::apply (double v) const
    {
        if (points.size() < 2) return v;   // identity

        const auto& first = points.front(), last = points.back();
        if (!(first.x <= last.x)) return v;   // malformed: treat as identity
        if (v <= first.x) return first.y;     // clamp below the curve
        if (v >= last.x)  return last.y;      // clamp above the curve

        for (size_t i = 1; i < points.size(); ++i)
            if (points[i].x >= v)
            {
                const auto& a = points[i - 1], b = points[i];
                const double span = b.x - a.x;
                return span > 1e-12 ? a.y + (b.y - a.y) * (v - a.x) / span : b.y;
            }
        return last.y;
    }

    // ============================================================ expressions

    /** Recursive-descent parser for the expression grammar:
        expr   := term (('+'|'-') term)*
        term   := factor (('*'|'/') factor)*
        factor := ('-'|'+') factor | primary
        primary:= NUMBER | IDENT ('.' IDENT)? | '(' expr ')'
        (friend of Expression: see the friend declaration in ModelSession.h) */
    struct ExpressionParser
    {
        const juce::String& s;
        size_t i = 0;
        bool failed = false;

        explicit ExpressionParser (const juce::String& text) : s (text) {}

        void skipWs()
        {
            while (i < s.length() && (s[i] == ' ' || s[i] == '\t')) ++i;
        }
        char peek() const { return i < s.length() ? s[i] : '\0'; }
        bool isIdentStart (char c) const
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }
        bool isDigit (char c) const { return c >= '0' && c <= '9'; }

        std::shared_ptr<Expression::Node> parseExpr()
        {
            auto node = parseTerm();
            if (!node) return nullptr;
            for (;;)
            {
                skipWs();
                const char c = peek();
                if (c != '+' && c != '-') break;
                ++i;
                auto rhs = parseTerm();
                if (!rhs) return nullptr;
                auto op = std::make_shared<Expression::Node>();
                op->kind = c == '+' ? Expression::Node::Kind::Add : Expression::Node::Kind::Sub;
                op->a = node; op->b = rhs;
                node = op;
            }
            return node;
        }

        std::shared_ptr<Expression::Node> parseTerm()
        {
            auto node = parseFactor();
            if (!node) return nullptr;
            for (;;)
            {
                skipWs();
                const char c = peek();
                if (c != '*' && c != '/') break;
                ++i;
                auto rhs = parseFactor();
                if (!rhs) return nullptr;
                auto op = std::make_shared<Expression::Node>();
                op->kind = c == '*' ? Expression::Node::Kind::Mul : Expression::Node::Kind::Div;
                op->a = node; op->b = rhs;
                node = op;
            }
            return node;
        }

        std::shared_ptr<Expression::Node> parseFactor()
        {
            skipWs();
            const char c = peek();
            if (c == '-' || c == '+')
            {
                ++i;
                auto inner = parseFactor();
                if (!inner) return nullptr;
                if (c != '-') return inner;   // unary plus is a no-op
                auto neg = std::make_shared<Expression::Node>();
                neg->kind = Expression::Node::Kind::Neg;
                neg->a = inner;
                return neg;
            }
            return parsePrimary();
        }

        std::shared_ptr<Expression::Node> parsePrimary()
        {
            skipWs();
            const char c = peek();
            if (c == '\0') { failed = true; return nullptr; }

            if (c == '(')
            {
                ++i;
                auto inner = parseExpr();
                skipWs();
                if (peek() != ')') { failed = true; return nullptr; }
                ++i;
                return inner;
            }

            if (isDigit (c))
            {
                size_t start = i;
                while (i < s.length() && isDigit (s[i])) ++i;
                // A dot belongs to the number only when a digit follows it.
                if (i + 1 < s.length() && s[i] == '.' && isDigit (s[i + 1]))
                {
                    ++i;   // consume the dot
                    while (i < s.length() && isDigit (s[i])) ++i;
                }
                // The scanner only accepts digits and at most one dot, so the
                // substring is always a valid number literal.
                const double v = s.substring (start, i).getDoubleValue ();
                auto node = std::make_shared<Expression::Node>();
                node->kind = Expression::Node::Kind::Number;
                node->num = v;
                return node;
            }

            if (isIdentStart (c))
            {
                size_t start = i;
                while (i < s.length() && (isDigit (s[i]) || isIdentStart (s[i]))) ++i;
                juce::String name = s.substring (start, i);
                // Qualified form: IDENT '.' IDENT (the dot must be followed by an identifier).
                if (i + 1 < s.length() && s[i] == '.' && isIdentStart (s[i + 1]))
                {
                    ++i;   // consume the dot
                    while (i < s.length() && (isDigit (s[i]) || isIdentStart (s[i]))) ++i;
                    name = s.substring (start, i);   // "Table.Param"
                }
                auto node = std::make_shared<Expression::Node>();
                node->kind = Expression::Node::Kind::Param;
                node->param = name;
                return node;
            }

            failed = true;   // unexpected character
            return nullptr;
        }
    };

    bool Expression::parse (const juce::String& text, Expression& out, juce::String* errOut)
    {
        out.root_ = nullptr;
        const juce::String trimmed = text.trim();
        if (trimmed.isEmpty()) return true;   // empty expression is valid

        ExpressionParser p (trimmed);
        auto root = p.parseExpr();
        if (!root || p.failed)
        {
            if (errOut != nullptr) *errOut = "Syntax error in expression";
            return false;
        }
        p.skipWs();
        if (p.peek() != '\0')
        {
            if (errOut != nullptr) *errOut = "Unexpected trailing characters";
            return false;
        }
        out.root_ = root;
        return true;
    }

    Expression::Result Expression::evaluate (const ParamResolver& resolve) const
    {
        Result r;
        if (root_ == nullptr) return r;   // empty -> ok, value 0

        std::function<double(const Node*)> eval = [&](const Node* n) -> double
        {
            switch (n->kind)
            {
                case Node::Kind::Number: return n->num;
                case Node::Kind::Param:
                {
                    bool found = false;
                    const double v = resolve ? resolve (n->param, &found) : 0.0;
                    if (!found || !std::isfinite (v))
                    {
                        r.ok = false;
                        r.error = "Parameter not found: " + n->param;
                        return 0.0;
                    }
                    return v;
                }
                case Node::Kind::Neg:   return -eval (n->a.get());
                case Node::Kind::Add:   return eval (n->a.get()) + eval (n->b.get());
                case Node::Kind::Sub:   return eval (n->a.get()) - eval (n->b.get());
                case Node::Kind::Mul:   return eval (n->a.get()) * eval (n->b.get());
                case Node::Kind::Div:
                {
                    const double d = eval (n->b.get());
                    if (!r.ok) return 0.0;
                    if (std::abs (d) < 1e-300)
                    {
                        r.ok = false;
                        r.error = "Division by zero";
                        return 0.0;
                    }
                    return eval (n->a.get()) / d;
                }
            }
            return 0.0;
        };

        const double v = eval (root_.get());
        if (!r.ok) return r;
        if (!std::isfinite (v))
        {
            r.ok = false;
            r.error = "Expression overflows";
            return r;
        }
        r.value = v;
        return r;
    }

    void Expression::collectParams (const Node* n, std::vector<juce::String>& out)
    {
        if (n == nullptr) return;
        switch (n->kind)
        {
            case Node::Kind::Param:
                if (std::find (out.begin(), out.end(), n->param) == out.end())
                    out.push_back (n->param);
                break;
            default:
                collectParams (n->a.get (), out);
                collectParams (n->b.get (), out);
                break;
        }
    }

    std::vector<juce::String> Expression::params() const
    {
        std::vector<juce::String> out;
        collectParams (root_.get(), out);
        return out;
    }

    // ============================================================ grid mapping

    double axisIndex (const std::vector<double>& breaks, double v)
    {
        const int n = (int) breaks.size() + 1;
        if (n == 1) return 0.0;

        // Interpolation node per cell: midpoints between surrounding breakpoints;
        // edge nodes extrapolated by half the adjacent interior cell width
        // (unit width when there is exactly one breakpoint).
        std::vector<double> nodes ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            if (i == 0)
            {
                const double w = breaks.size() >= 2 ? breaks[1] - breaks[0] : 1.0;
                nodes[(size_t) i] = breaks[0] - w / 2.0;
            }
            else if (i == n - 1)
            {
                const double w = breaks.size() >= 2
                    ? breaks.back () - breaks[breaks.size () - 2] : 1.0;
                nodes[(size_t) i] = breaks.back () + w / 2.0;
            }
            else
                nodes[(size_t) i] = (breaks[(size_t) (i - 1)] + breaks[(size_t) i]) * 0.5;
        }

        const double x = std::clamp (v, nodes.front (), nodes.back ());
        for (int i = 0; i < n - 1; ++i)
            if (x <= nodes[(size_t) (i + 1)])
            {
                const double span = nodes[(size_t) (i + 1)] - nodes[(size_t) i];
                return span > 1e-12 ? i + (x - nodes[(size_t) i]) / span : (double) i;
            }
        return n - 1;
    }

    BilinearResult bilinearWeights (const std::vector<double>& xBreaks,
                                    const std::vector<double>& yBreaks,
                                    int numCols, int numRows,
                                    double x, double y,
                                    const std::function<bool(int row, int col)>& isEmpty)
    {
        BilinearResult r;
        if (numCols <= 0 || numRows <= 0)
        {
            r.w[0] = r.w[1] = r.w[2] = r.w[3] = 0.0;   // no cells at all
            return r;
        }

        const double ci = axisIndex (xBreaks, x);
        const double ri = axisIndex (yBreaks, y);

        int c0 = (int) std::floor (ci), r0 = (int) std::floor (ri);
        double fx, fy;
        if (numCols == 1)      { c0 = 0; fx = 0.0; }
        else if (c0 >= numCols - 1) { c0 = numCols - 2; fx = 1.0; }
        else                   { fx = ci - c0; }
        if (numRows == 1)      { r0 = 0; fy = 0.0; }
        else if (r0 >= numRows - 1) { r0 = numRows - 2; fy = 1.0; }
        else                   { fy = ri - r0; }

        auto cellEmpty = [&](int rr, int cc)
        {
            return rr < 0 || cc < 0 || rr >= numRows || cc >= numCols
                || (isEmpty != nullptr && isEmpty (rr, cc));
        };

        double w[4] = { (1.0 - fx) * (1.0 - fy),   // (r0, c0)
                        fx * (1.0 - fy),          // (r0, c0+1)
                        (1.0 - fx) * fy,          // (r0+1, c0)
                        fx * fy };                // (r0+1, c0+1)
        if (cellEmpty (r0, c0))         w[0] = 0.0;
        if (cellEmpty (r0, c0 + 1))     w[1] = 0.0;
        if (cellEmpty (r0 + 1, c0))     w[2] = 0.0;
        if (cellEmpty (r0 + 1, c0 + 1)) w[3] = 0.0;

        double sum = w[0] + w[1] + w[2] + w[3];
        r.col = c0; r.row = r0;
        if (sum <= 1e-12)
        {
            r.w[0] = r.w[1] = r.w[2] = r.w[3] = 0.0;   // all four empty
            return r;
        }
        for (int i = 0; i < 4; ++i) r.w[i] = w[i] / sum;   // renormalise over non-empty cells
        return r;
    }

    // ============================================================ parameters

    double resolveParam (const std::vector<sc::tables::TableData>& tables,
                         const juce::String& name, double tSecs,
                         bool* found, bool* ambiguous)
    {
        if (found != nullptr) *found = false;
        if (ambiguous != nullptr) *ambiguous = false;

        // Qualified "TableName.ParamName" vs plain parameter name.
        juce::String tableName, paramName = name;
        const int dot = name.indexOfChar ('.');
        if (dot > 0 && dot < name.length() - 1)
        {
            tableName = name.substring (0, dot);
            paramName = name.substring (dot + 1);
        }

        const sc::tables::TableData* match = nullptr;
        int matches = 0;
        for (const auto& t : tables)
        {
            if (!tableName.isEmpty () && !t.name.equalsIgnoreCase (tableName)) continue;
            bool hasParam = false;
            for (const auto& p : t.params)
                if (p.name == paramName || p.name.equalsIgnoreCase (paramName))
                    { hasParam = true; break; }
            if (hasParam) { ++matches; if (match == nullptr) match = &t; }
        }
        if (match == nullptr) return 0.0;   // *found stays false

        if (ambiguous != nullptr && matches > 1 && tableName.isEmpty())
            *ambiguous = true;              // first match wins, caller may warn
        if (found != nullptr) *found = true;

        const sc::tables::ParamData* p = nullptr;
        for (const auto& pp : match->params)
            if (pp.name == paramName || pp.name.equalsIgnoreCase (paramName))
                { p = &pp; break; }
        jassert (p != nullptr);
        if (p == nullptr || p->values.empty ()) return 0.0;

        // Invert the effective time map: docTime = off + scale*tTable.
        const auto [off, scale] = match->effectiveTimeMap (*p);
        double tt = (std::abs (scale) > 1e-12) ? (tSecs - off) / scale : 0.0;

        // Interpolate in table time; outside the parameter's domain the nearest
        // endpoint value is held (model inputs must not drop to zero mid-run).
        const int n = (int) p->values.size();
        if (n == 1) return p->values[0];
        tt = std::clamp (tt, 0.0, (double) (n - 1) / match->sampleRate);
        const double pos = tt * match->sampleRate;
        int lo = std::min (n - 2, (int) pos);
        const double frac = pos - lo;
        return p->values[(size_t) lo] * (1.0 - frac) + p->values[(size_t) (lo + 1)] * frac;
    }

    // ============================================================ data model

    bool Cell::isEmptyForType (int componentType) const
    {
        if (componentType == 1)   // Noise: empty when it has no live range and no cached layers
            return detached ? (residualPower.empty () && rawPower.empty ()) : !(t1 > t0);
        return picks.empty ();    // Harmonic
    }

    Expression::Result ModelInput::evaluateAt (double tSecs, const Expression::ParamResolver& resolve) const
    {
        if (expression.trim ().isEmpty())
            return { true, curve.apply (defaultValue), "" };

        Expression e;
        juce::String err;
        if (!Expression::parse (expression, e, &err))
            return { false, defaultValue, err };

        const auto r = e.evaluate (resolve);
        if (!r.ok) return r;
        return { true, curve.apply (r.value), "" };
    }

    // ============================================================ session

    Component& ModelSession::addComponent (const juce::String& name, int type)
    {
        Component c;
        c.id = juce::Uuid ().toString ();
        c.name = name.trim ().isEmpty ()
            ? ("Component " + juce::String ((int) components.size () + 1))
            : name.trim ();
        c.type = (type == 1) ? 1 : 0;
        c.inputX.defaultValue    = 0.0;
        c.inputY.defaultValue    = 0.0;
        c.inputSpeed.defaultValue = 1.0;
        ModelInput amp;
        amp.defaultValue = 1.0;
        c.amplitudes.push_back (amp);
        components.push_back (std::move (c));
        dirty = true;
        return components.back ();
    }

    void ModelSession::removeComponent (size_t index)
    {
        if (index >= components.size ()) return;
        const juce::String id = components[index].id;
        components.erase (components.begin () + (long long) index);
        // Drop any subtraction entries that referenced the removed component.
        for (auto& c : components)
            for (auto& cell : c.cells)
                cell.subtractions.erase (std::remove_if (cell.subtractions.begin (),
                                                         cell.subtractions.end (),
                                                         [&id] (const SubOverride& so)
                                                         { return so.candidateId == id; }),
                                         cell.subtractions.end ());
        dirty = true;
    }

    void ModelSession::moveComponent (size_t from, size_t to)
    {
        if (from >= components.size () || to >= components.size () || from == to) return;
        Component c = std::move (components[from]);
        components.erase (components.begin () + (long long) from);
        components.insert (components.begin () + (long long) to, std::move (c));
        dirty = true;
    }

    const Component* ModelSession::findComponent (const juce::String& id) const
    {
        for (auto& c : components)
            if (c.id == id) return &c;
        return nullptr;
    }

    // ------------------------------------------------- time references

    namespace {
    bool sameFile (const juce::File& a, const juce::String& b)
    {
        return !b.isEmpty () && a == juce::File (b);
    }
    }   // namespace

    void ModelSession::applyTimelineEdit (const juce::File& file, TimelineEdit kind,
                                          double a, double b, double c)
    {
        auto Wdel = [a, b] (double t)
        {
            if (t < a) return t;
            if (t >= b) return t - (b - a);
            return a;   // inside the deleted range: collapse to its start
        };
        auto Wins  = [a, len = b] (double t) { return t < a ? t : t + len; };
        auto Wstr  = [a, bOld = b, bNew = c] (double t)
        {
            if (t <= a) return t;
            const double r = (bOld - a) > 1e-12 ? (bNew - a) / (bOld - a) : 0.0;
            return a + (t - a) * r;
        };

        bool changed = false;
        for (auto& c : components)
            for (auto& cell : c.cells)
            {
                // --- noise range (time references t0/t1) ---
                if (c.type == 1 && sameFile (file, cell.sourceFile))
                {
                    switch (kind)
                    {
                        case TimelineEdit::Deletion:
                            if (cell.t0 >= a && cell.t1 <= b)
                                cell.detached = true;   // fully deleted: keep cached layers
                            else
                            {
                                cell.t0 = Wdel (cell.t0);
                                cell.t1 = Wdel (cell.t1);
                                if (!(cell.t1 > cell.t0)) cell.detached = true;
                            }
                            break;
                        case TimelineEdit::Insertion:
                            cell.t0 = Wins (cell.t0);
                            cell.t1 = Wins (cell.t1);
                            break;
                        case TimelineEdit::Stretch:
                            cell.t0 = Wstr (cell.t0);
                            cell.t1 = Wstr (cell.t1);
                            if (!(cell.t1 > cell.t0)) cell.detached = true;
                            break;
                    }
                    changed = true;
                }

                // --- harmonic pick times ---
                if (c.type == 0)
                    for (auto& pk : cell.picks)
                    {
                        const juce::String src = pk.sourceFile.isEmpty () ? cell.sourceFile : pk.sourceFile;
                        if (!sameFile (file, src)) continue;
                        if (pk.timeRemoved) continue;   // already flagged: keep its values

                        switch (kind)
                        {
                            case TimelineEdit::Deletion:
                                if (pk.timeSecs >= a && pk.timeSecs < b)
                                    pk.timeRemoved = true;
                                else
                                    pk.timeSecs = Wdel (pk.timeSecs);
                                break;
                            case TimelineEdit::Insertion:
                                pk.timeSecs = Wins (pk.timeSecs);
                                break;
                            case TimelineEdit::Stretch:
                                pk.timeSecs = Wstr (pk.timeSecs);
                                break;
                        }
                        changed = true;
                    }
            }

        if (changed) dirty = true;
    }

    namespace {
    /** A var wrapping a fresh DynamicObject (a JSON object). */
    juce::var makeObj () { return juce::var (new juce::DynamicObject ()); }

    void setProp (juce::var& v, const juce::String& key, const juce::var& value)
    {
        if (auto* d = v.getDynamicObject()) d->setProperty (key, value);
    }
    }   // namespace

    juce::String ModelSession::serialiseTimeRefsFor (const juce::File& file) const
    {
        juce::Array<juce::var> arr;
        bool any = false;
        for (size_t ci = 0; ci < components.size (); ++ci)
            for (size_t ce = 0; ce < components[ci].cells.size (); ++ce)
            {
                const Cell& cell = components[ci].cells[ce];
                if (!sameFile (file, cell.sourceFile)) continue;

                any = true;
                auto e = makeObj ();
                setProp (e, "comp", (int) ci);
                setProp (e, "cell", (int) ce);
                if (components[ci].type == 1)
                {
                    setProp (e, "t0", cell.t0);
                    setProp (e, "t1", cell.t1);
                    setProp (e, "detached", cell.detached);
                }
                if (!cell.picks.empty ())
                {
                    juce::Array<juce::var> pks;
                    for (size_t pi = 0; pi < cell.picks.size (); ++pi)
                    {
                        auto pe = makeObj ();
                        setProp (pe, "i", (int) pi);
                        setProp (pe, "timeSecs", cell.picks[pi].timeSecs);
                        setProp (pe, "removed", cell.picks[pi].timeRemoved);
                        pks.add (pe);
                    }
                    setProp (e, "picks", juce::var (pks));
                }
                arr.add (e);
            }

        return any ? juce::JSON::toString (juce::var (arr)) : "";
    }

    void ModelSession::applyTimeRefsFor (const juce::File& file, const juce::String& json)
    {
        if (json.trim ().isEmpty ()) return;
        const auto root = juce::JSON::parse (json);
        const auto* arr = root.getArray();
        if (arr == nullptr) return;

        for (const auto& e : *arr)
        {
            const int ci = (int) e.getProperty ("comp", -1);
            const int ce = (int) e.getProperty ("cell", -1);
            if (ci < 0 || ci >= (int) components.size ()) continue;
            if (ce < 0 || ce >= (int) components[ci].cells.size ()) continue;

            Cell& cell = components[(size_t) ci].cells[(size_t) ce];
            if (!sameFile (file, cell.sourceFile)) continue;   // defensive: refs are per-file

            if (e.hasProperty ("t0")) cell.t0 = (double) e.getProperty ("t0", 0.0);
            if (e.hasProperty ("t1")) cell.t1 = (double) e.getProperty ("t1", 0.0);
            if (e.hasProperty ("detached")) cell.detached = (bool) e.getProperty ("detached", false);

            const auto* pks = e.getProperty ("picks", juce::var()).getArray();
            if (pks != nullptr)
                for (const auto& pe : *pks)
                {
                    const int pi = (int) pe.getProperty ("i", -1);
                    if (pi < 0 || pi >= (int) cell.picks.size ()) continue;
                    cell.picks[(size_t) pi].timeSecs    = (double) pe.getProperty ("timeSecs", 0.0);
                    cell.picks[(size_t) pi].timeRemoved = (bool) pe.getProperty ("removed", false);
                }
        }

        dirty = true;
    }

    // ============================================================ JSON I/O

    namespace {
    juce::var curveToVar (const RemapCurve& c)
    {
        auto v = makeObj ();
        juce::Array<juce::var> pts;
        for (const auto& p : c.points)
        {
            auto pt = juce::var (juce::Array<juce::var>());
            pt.append (p.x);
            pt.append (p.y);
            pts.add (pt);
        }
        setProp (v, "curve", juce::var (pts));
        setProp (v, "axisXLo", c.axisXLo);
        setProp (v, "axisXHi", c.axisXHi);
        setProp (v, "axisYLo", c.axisYLo);
        setProp (v, "axisYHi", c.axisYHi);
        return v;
    }

    RemapCurve curveFromVar (const juce::var& v)
    {
        RemapCurve c;
        const auto* pts = v.getProperty ("curve", juce::var()).getArray();
        if (pts != nullptr)
            for (const auto& pt : *pts)
                if (pt.isArray () && pt.size () >= 2)
                    c.points.push_back ({ (double) pt[0], (double) pt[1] });
        std::sort (c.points.begin (), c.points.end (),
                   [] (const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
        c.axisXLo = (double) v.getProperty ("axisXLo", 0.0);
        c.axisXHi = (double) v.getProperty ("axisXHi", 1.0);
        c.axisYLo = (double) v.getProperty ("axisYLo", 0.0);
        c.axisYHi = (double) v.getProperty ("axisYHi", 1.0);
        return c;
    }

    juce::var inputToVar (const ModelInput& in)
    {
        auto v = makeObj ();
        setProp (v, "expr", in.expression);
        setProp (v, "curve", curveToVar (in.curve));
        return v;
    }

    ModelInput inputFromVar (const juce::var& v, double defaultValue)
    {
        ModelInput in;
        in.defaultValue = defaultValue;
        in.expression = v.getProperty ("expr", "").toString ();
        in.curve = curveFromVar (v.getProperty ("curve", juce::var()));
        return in;
    }

    juce::var pickToVar (const HarmonicPick& pk, const juce::File& sessionDir)
    {
        auto v = makeObj ();
        setProp (v, "freqHz", pk.freqHz);
        setProp (v, "widthHz", pk.widthHz);
        setProp (v, "sourceFile", pk.sourceFile);   // absolute: picks are per-file measurements
        setProp (v, "channel", pk.channel);
        setProp (v, "timeSecs", pk.timeSecs);
        setProp (v, "fftSize", pk.fftSize);
        setProp (v, "windowType", pk.windowType);
        setProp (v, "binHz", pk.binHz);
        setProp (v, "peakBin", pk.peakBin);
        juce::Array<juce::var> lp;
        for (float p : pk.localPower) lp.add ((double) p);
        setProp (v, "localPower", juce::var (lp));
        setProp (v, "rawPeakAmp", pk.rawPeakAmp);
        setProp (v, "timeRemoved", pk.timeRemoved);
        return v;
    }

    HarmonicPick pickFromVar (const juce::var& v)
    {
        HarmonicPick pk;
        pk.freqHz      = (double) v.getProperty ("freqHz", 0.0);
        pk.widthHz     = (double) v.getProperty ("widthHz", 50.0);
        pk.sourceFile  = v.getProperty ("sourceFile", "").toString ();
        pk.channel     = (int) v.getProperty ("channel", 0);
        pk.timeSecs    = (double) v.getProperty ("timeSecs", 0.0);
        pk.fftSize     = (int) v.getProperty ("fftSize", 4096);
        pk.windowType  = (int) v.getProperty ("windowType", 1);
        pk.binHz       = (double) v.getProperty ("binHz", 0.0);
        pk.peakBin     = (int) v.getProperty ("peakBin", -1);
        const auto* lp = v.getProperty ("localPower", juce::var()).getArray();
        if (lp != nullptr)
            for (const auto& e : *lp) pk.localPower.push_back ((float) e);
        pk.rawPeakAmp  = (double) v.getProperty ("rawPeakAmp", 0.0);
        pk.timeRemoved = (bool) v.getProperty ("timeRemoved", false);
        return pk;
    }

    juce::String relPath (const juce::File& sessionDir, const juce::String& full)
    {
        if (full.isEmpty ()) return {};
        auto f = juce::File (full);
        const auto rel = f.getRelativePathFrom (sessionDir);
        return rel.isEmpty () ? full : rel;   // unrelated trees: keep the absolute path
    }

    juce::File resolvePath (const juce::File& sessionDir, const juce::String& p)
    {
        if (p.isEmpty ()) return {};
        auto rel = sessionDir.getChildFile (p);
        if (rel.existsAsFile ()) return rel;
        auto abs = juce::File (p);
        return abs.existsAsFile () ? abs : rel;   // keep the relative candidate when both miss
    }

    void powerToVarArr (const std::vector<float>& v, juce::Array<juce::var>& out)
    {
        for (float p : v) out.add ((double) p);
    }
    std::vector<float> varArrToPower (const juce::var& v)
    {
        std::vector<float> out;
        const auto* arr = v.getArray();
        if (arr != nullptr)
            for (const auto& e : *arr) out.push_back ((float) e);
        return out;
    }

    juce::var cellToVar (const Cell& cell, int type, const juce::File& sessionDir)
    {
        auto v = makeObj ();
        setProp (v, "sourceFile", relPath (sessionDir, cell.sourceFile));

        if (type == 0)   // Harmonic
        {
            juce::Array<juce::var> pks;
            for (const auto& pk : cell.picks) pks.add (pickToVar (pk, sessionDir));
            setProp (v, "picks", juce::var (pks));
            setProp (v, "refSpeed", cell.refSpeed);
            setProp (v, "refSpeedFromFirstPick", cell.refSpeedFromFirstPick);

            juce::Array<juce::var> res;
            for (const auto& pk : cell.residualPeaks) res.add (pickToVar (pk, sessionDir));
            setProp (v, "residualPeaks", juce::var (res));
        }
        else             // Noise
        {
            setProp (v, "t0", cell.t0);
            setProp (v, "t1", cell.t1);
            setProp (v, "channel", cell.channel);
            setProp (v, "fftSize", cell.fftSize);
            setProp (v, "windowType", cell.windowType);
            setProp (v, "detached", cell.detached);
            juce::Array<juce::var> raw, resP;
            powerToVarArr (cell.rawPower, raw);
            powerToVarArr (cell.residualPower, resP);
            setProp (v, "rawPower", juce::var (raw));
            setProp (v, "rawBinHz", cell.rawBinHz);
            setProp (v, "residualPower", juce::var (resP));

            juce::Array<juce::var> res;
            for (const auto& pk : cell.residualPeaks) res.add (pickToVar (pk, sessionDir));
            setProp (v, "residualPeaks", juce::var (res));
        }

        if (!cell.subtractions.empty ())
        {
            juce::Array<juce::var> subs;
            for (const auto& so : cell.subtractions)
            {
                auto sv = makeObj ();
                setProp (sv, "candidateId", so.candidateId);
                setProp (sv, "bypass", so.bypass);
                if (!so.overrides.empty ())
                {
                    auto ov = makeObj ();
                    for (const auto& [k, val] : so.overrides)
                        setProp (ov, juce::String (k), juce::String (val));
                    setProp (sv, "overrides", ov);
                }
                subs.add (sv);
            }
            setProp (v, "subtractions", juce::var (subs));
        }
        return v;
    }

    Cell cellFromVar (const juce::var& v, int type, const juce::File& sessionDir)
    {
        Cell cell;
        cell.sourceFile = resolvePath (sessionDir, v.getProperty ("sourceFile", "").toString()).getFullPathName ();

        if (type == 0)   // Harmonic
        {
            const auto* pks = v.getProperty ("picks", juce::var()).getArray();
            if (pks != nullptr)
                for (const auto& e : *pks) cell.picks.push_back (pickFromVar (e));
            cell.refSpeed = (double) v.getProperty ("refSpeed", 0.0);
            cell.refSpeedFromFirstPick = (bool) v.getProperty ("refSpeedFromFirstPick", false);

            const auto* res = v.getProperty ("residualPeaks", juce::var()).getArray();
            if (res != nullptr)
                for (const auto& e : *res) cell.residualPeaks.push_back (pickFromVar (e));
        }
        else             // Noise
        {
            cell.t0 = (double) v.getProperty ("t0", 0.0);
            cell.t1 = (double) v.getProperty ("t1", 0.0);
            cell.channel = (int) v.getProperty ("channel", 0);
            cell.fftSize = (int) v.getProperty ("fftSize", 4096);
            cell.windowType = (int) v.getProperty ("windowType", 1);
            cell.detached = (bool) v.getProperty ("detached", false);
            cell.rawPower = varArrToPower (v.getProperty ("rawPower", juce::var()));
            cell.rawBinHz = (double) v.getProperty ("rawBinHz", 0.0);
            cell.residualPower = varArrToPower (v.getProperty ("residualPower", juce::var()));

            const auto* res = v.getProperty ("residualPeaks", juce::var()).getArray();
            if (res != nullptr)
                for (const auto& e : *res) cell.residualPeaks.push_back (pickFromVar (e));
        }

        const auto* subs = v.getProperty ("subtractions", juce::var()).getArray();
        if (subs != nullptr)
            for (const auto& sv : *subs)
            {
                SubOverride so;
                so.candidateId = sv.getProperty ("candidateId", "").toString ();
                so.bypass = (bool) sv.getProperty ("bypass", false);
                const auto* ov = sv.getProperty ("overrides", juce::var()).getDynamicObject();
                if (ov != nullptr)
                    for (const auto& nv : ov->getProperties ())
                        so.overrides[nv.name.toString().toStdString()] = nv.value.toString().toStdString ();
                cell.subtractions.push_back (std::move (so));
            }
        return cell;
    }

    juce::var componentToVar (const Component& c, const juce::File& sessionDir)
    {
        auto v = makeObj ();
        setProp (v, "id", c.id);
        setProp (v, "name", c.name);
        setProp (v, "comment", c.comment);
        setProp (v, "type", c.type);
        setProp (v, "inputX", inputToVar (c.inputX));
        setProp (v, "inputY", inputToVar (c.inputY));
        setProp (v, "inputSpeed", inputToVar (c.inputSpeed));

        juce::Array<juce::var> amps;
        for (const auto& a : c.amplitudes) amps.add (inputToVar (a));
        setProp (v, "amplitudes", juce::var (amps));

        juce::Array<juce::var> xb, yb;
        for (double b : c.xBreaks) xb.add (b);
        for (double b : c.yBreaks) yb.add (b);
        setProp (v, "xBreaks", juce::var (xb));
        setProp (v, "yBreaks", juce::var (yb));

        juce::Array<juce::var> cells;
        for (const auto& cell : c.cells) cells.add (cellToVar (cell, c.type, sessionDir));
        setProp (v, "cells", juce::var (cells));
        return v;
    }

    Component componentFromVar (const juce::var& v, const juce::File& sessionDir)
    {
        Component c;
        c.id = v.getProperty ("id", "").toString ();
        if (c.id.isEmpty ()) c.id = juce::Uuid ().toString ();
        c.name = v.getProperty ("name", "Component").toString ();
        c.comment = v.getProperty ("comment", "").toString ();
        c.type = (int) v.getProperty ("type", 0);

        c.inputX = inputFromVar (v.getProperty ("inputX", juce::var()), 0.0);
        c.inputY = inputFromVar (v.getProperty ("inputY", juce::var()), 0.0);
        c.inputSpeed = inputFromVar (v.getProperty ("inputSpeed", juce::var()), 1.0);

        const auto* amps = v.getProperty ("amplitudes", juce::var()).getArray();
        if (amps != nullptr)
            for (const auto& e : *amps) c.amplitudes.push_back (inputFromVar (e, 1.0));
        if (c.amplitudes.empty ()) { ModelInput a; a.defaultValue = 1.0; c.amplitudes.push_back (a); }

        const auto* xb = v.getProperty ("xBreaks", juce::var()).getArray();
        if (xb != nullptr) for (const auto& e : *xb) c.xBreaks.push_back ((double) e);
        const auto* yb = v.getProperty ("yBreaks", juce::var()).getArray();
        if (yb != nullptr) for (const auto& e : *yb) c.yBreaks.push_back ((double) e);

        const auto* cells = v.getProperty ("cells", juce::var()).getArray();
        if (cells != nullptr)
            for (const auto& e : *cells) c.cells.push_back (cellFromVar (e, c.type, sessionDir));
        return c;
    }
    }   // namespace

    juce::var ModelSession::toVar (const ModelSession& s, const juce::File& sessionDir)
    {
        auto root = makeObj ();
        setProp (root, "format", "spectrocraft-model");
        setProp (root, "version", kFormatVersion);
        setProp (root, "matchTolerancePct", s.matchTolerancePct);
        setProp (root, "inclusionThresholdDb", s.inclusionThresholdDb);
        setProp (root, "overSubtractToleranceDb", s.overSubtractToleranceDb);

        juce::Array<juce::var> comps;
        for (const auto& c : s.components) comps.add (componentToVar (c, sessionDir));
        setProp (root, "components", juce::var (comps));
        return root;
    }

    bool ModelSession::fromVar (ModelSession& s, const juce::var& root,
                                const juce::File& sessionDir, juce::String* errOut)
    {
        if (!root.isObject ())
        {
            if (errOut != nullptr) *errOut = "Not a model file";
            return false;
        }
        if (root.getProperty ("format", "").toString () != "spectrocraft-model")
        {
            if (errOut != nullptr) *errOut = "Unknown format string";
            return false;
        }
        const int version = (int) root.getProperty ("version", 0);
        if (version < kFormatVersion)
        {
            if (errOut != nullptr) *errOut = "Unsupported model file version: " + juce::String (version);
            return false;
        }

        s.filePath.clear ();
        s.dirty = false;
        s.matchTolerancePct      = (double) root.getProperty ("matchTolerancePct", 3.0);
        s.inclusionThresholdDb   = (double) root.getProperty ("inclusionThresholdDb", -60.0);
        s.overSubtractToleranceDb = (double) root.getProperty ("overSubtractToleranceDb", 1.0);
        s.components.clear ();

        const auto* comps = root.getProperty ("components", juce::var()).getArray();
        if (comps != nullptr)
            for (const auto& e : *comps)
                s.components.push_back (componentFromVar (e, sessionDir));
        return true;
    }

    bool ModelSession::saveToFile (const ModelSession& s, const juce::File& target)
    {
        if (!target.getParentDirectory ().isDirectory ())
            if (!target.getParentDirectory ().createDirectory())
                return false;
        // Source-file paths are stored relative to the directory that will
        // contain the session file.
        const auto root = toVar (s, target.getParentDirectory ());
        return target.replaceWithText (juce::JSON::toString (root));
    }

    bool ModelSession::loadFromFile (ModelSession& s, const juce::File& source, juce::String* errOut)
    {
        if (!source.existsAsFile ())
        {
            if (errOut != nullptr) *errOut = "File not found";
            return false;
        }
        const auto root = juce::JSON::parse (source.loadFileAsString());
        if (root.isVoid ())
        {
            if (errOut != nullptr) *errOut = "Not valid JSON";
            return false;
        }
        ModelSession tmp;
        if (!fromVar (tmp, root, source.getParentDirectory (), errOut))
            return false;
        s = std::move (tmp);
        s.filePath = source.getFullPathName ();
        return true;
    }

    // ============================================================ analysis DSP

    void makeWindow (int n, int windowType, std::vector<float>& win)
    {
        win.resize ((size_t) std::max (0, n));
        if (n <= 0) return;
        const double pi = juce::MathConstants<double>::pi;
        for (int i = 0; i < n; ++i)
            switch (windowType)
            {
                case 1:   // Hann
                    win[(size_t) i] = (float) (0.5 * (1.0 - std::cos (2.0 * pi * i / n)));
                    break;
                case 2:   // Hamming
                    win[(size_t) i] = (float) (n > 1 ? 0.54 - 0.46 * std::cos (2.0 * pi * i / (n - 1)) : 1.0);
                    break;
                default:  // Rectangular
                    win[(size_t) i] = 1.0f;
                    break;
            }
    }

    namespace {
    /** Frame starts covering [s0,s1]: hop fftSize/4, capped so a long range
        still averages at most kMaxFrames frames (mirrors the classic spectrum
        path's curveFrameStarts). */
    constexpr int64_t kMaxAnalysisFrames = 1024;

    std::vector<int64_t> analysisFrameStarts (int64_t s0, int64_t s1, int fftSize)
    {
        std::vector<int64_t> starts;
        const int64_t avail = s1 - s0;
        if (avail <= fftSize)
        {
            starts.push_back ((s0 + s1) / 2);   // single centred frame
            return starts;
        }
        const int64_t hop = std::max<int64_t> (fftSize / 4, (avail + kMaxAnalysisFrames - 1) / kMaxAnalysisFrames);
        for (int64_t s = s0; s + fftSize <= s1 && (int64_t) starts.size () < kMaxAnalysisFrames * 2; s += hop)
            starts.push_back (s);
        if (starts.empty ()) starts.push_back (s0);
        return starts;
    }

    int log2Int (int n)
    {
        int o = 0;
        while ((1 << o) < n) ++o;
        return o;
    }

    /** One windowed FFT of channel ch at sample start: fills buf (n floats,
        zero-filled beyond the end), applies win, runs the real FFT. */
    bool fftFrame (const FrameReader& src, int ch, int64_t start, int n,
                   const std::vector<float>& win, float* buf)
    {
        if (!src.readChannel (ch, start, n, buf)) return false;
        for (int i = 0; i < n; ++i) buf[i] *= win[(size_t) i];
        scfft::realForward (buf, log2Int (n));
        return true;
    }
    }   // namespace

    bool analyseNoiseRange (const FrameReader& src, int ch, double t0, double t1,
                            int fftSize, int windowType,
                            std::vector<float>& outPower, double& outBinHz)
    {
        const double rate = src.sampleRate ();
        if (!(rate > 0.0) || ch < 0 || ch >= src.numChannels () || !(t1 > t0)) return false;
        if (fftSize < 8 || (fftSize & (fftSize - 1)) != 0) return false;   // power of two

        const int64_t total = src.numSamples ();
        const int64_t s0 = std::clamp ((int64_t) (t0 * rate), (int64_t) 0, total);
        const int64_t s1 = std::clamp ((int64_t) (t1 * rate), s0, total);
        if (s1 <= s0) return false;

        std::vector<float> win;
        makeWindow (fftSize, windowType, win);
        double sumW = 0.0;
        for (float w : win) sumW += w;
        if (!(sumW > 0.0)) return false;
        const double ampScale = std::sqrt (2.0) / sumW;   // full-scale sine -> 1/sqrt(2) RMS

        const auto starts = analysisFrameStarts (s0, s1, fftSize);
        std::vector<double> acc ((size_t) fftSize / 2 + 1, 0.0);
        float* buf = new float[(size_t) fftSize * 2];   // realForward needs 2*N floats
        int64_t usedFrames = 0;
        for (int64_t st : starts)
        {
            const int64_t start = std::clamp (st, (int64_t) 0, total - fftSize);
            if (!fftFrame (src, ch, start, fftSize, win, buf)) continue;
            for (int k = 0; k <= fftSize / 2; ++k)
                acc[(size_t) k] += std::hypot ((double) buf[k * 2], (double) buf[k * 2 + 1]);
            ++usedFrames;
        }
        delete[] buf;
        if (usedFrames == 0) return false;

        const double numFrames = (double) usedFrames;
        outPower.resize ((size_t) fftSize / 2 + 1);
        for (int k = 0; k <= fftSize / 2; ++k)
        {
            // Same normalisation as the classic spectrum path: averaged single-sided RMS.
            const double lin = acc[(size_t) k] * ampScale / numFrames;
            outPower[(size_t) k] = (float) (lin * lin);   // linear power
        }
        outBinHz = rate / fftSize;
        return true;
    }

    bool analyseLocalPeak (const FrameReader& src, int ch, double timeSecs,
                           double freqHz, double widthHz, int fftSize, int windowType,
                           std::vector<float>& outPower, double& outBinHz, int& outPeakBin)
    {
        const double rate = src.sampleRate ();
        if (!(rate > 0.0) || ch < 0 || ch >= src.numChannels ()) return false;
        if (fftSize < 8 || (fftSize & (fftSize - 1)) != 0) return false;

        const int64_t total = src.numSamples ();
        if (total < fftSize) return false;

        // Frame centred on the pick time.
        const int64_t mid = (int64_t) std::llround (timeSecs * rate);
        const int64_t start = std::clamp (mid - fftSize / 2, (int64_t) 0, total - fftSize);

        std::vector<float> win;
        makeWindow (fftSize, windowType, win);
        double sumW = 0.0;
        for (float w : win) sumW += w;
        if (!(sumW > 0.0)) return false;
        const double ampScale = std::sqrt (2.0) / sumW;

        float* buf = new float[(size_t) fftSize * 2];
        bool ok = fftFrame (src, ch, start, fftSize, win, buf);
        if (!ok) { delete[] buf; return false; }

        const double binHz = rate / fftSize;
        outBinHz = binHz;

        // Find the true peak bin (k >= 1), then cache a window around it.
        int peakIdx = 1;
        double best = -1.0;
        for (int k = 1; k <= fftSize / 2; ++k)
        {
            const double mag = std::hypot ((double) buf[k * 2], (double) buf[k * 2 + 1]);
            if (mag > best) { best = mag; peakIdx = k; }
        }

        // Cache half-width: at least 3x the pick width, and at least 8 bins.
        const int half = std::max ((int) std::ceil (3.0 * std::max (1e-9, widthHz) / binHz), 8);
        const int lo = std::max (0, peakIdx - half);
        const int hi = std::min (fftSize / 2, peakIdx + half);

        outPower.clear ();
        for (int k = lo; k <= hi; ++k)
        {
            const double lin = std::hypot ((double) buf[k * 2], (double) buf[k * 2 + 1]) * ampScale;
            outPower.push_back ((float) (lin * lin));   // linear power, single frame
        }
        delete[] buf;

        outPeakBin = peakIdx - lo;
        (void) freqHz;   // the measured frequency is stored on the pick itself
        return true;
    }

}   // namespace sc::model
