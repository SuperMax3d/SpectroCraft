#include "SmMarkersPdf.h"
#include "SmTagDetector.h"

namespace scm::smap
{
namespace
{
constexpr double kPtPerMm = 72.0 / 25.4;

juce::String num (double v)
{
    return juce::String (v, 3);
}

juce::String pdfText (const juce::String& s)
{
    return s.replace ("\\", "\\\\").replace ("(", "\\(").replace (")", "\\)");
}

juce::String text (double xMm, double yMm, double sizePt, const juce::String& s, bool bold = false)
{
    return "BT /" + juce::String (bold ? "F2" : "F1") + " " + num (sizePt) + " Tf " + num (xMm * kPtPerMm) + " "
           + num (yMm * kPtPerMm) + " Td (" + pdfText (s) + ") Tj ET\n";
}

juce::String rectPath (double xMm, double yMm, double wMm, double hMm)
{
    return num (xMm * kPtPerMm) + " " + num (yMm * kPtPerMm) + " " + num (wMm * kPtPerMm) + " " + num (hMm * kPtPerMm) + " re\n";
}

juce::String rect (double xMm, double yMm, double wMm, double hMm)
{
    return rectPath (xMm, yMm, wMm, hMm) + "f\n";
}

/** Approximate Helvetica width (mm) to centre a line. */
double textWidthMm (const juce::String& s, double sizePt)
{
    return 0.52 * sizePt * (double) s.length() / kPtPerMm;
}

juce::String pageContent (int id, double S, const Paper& paper, const juce::String& title)
{
    const auto cells = TagDetector::tagCells (id);
    const double cell = S / 8.0;
    const double W = paper.widthMm, H = paper.heightMm;
    const double x0 = 0.5 * (W - S);                 // left edge of the black square
    const double yTop = H - 0.5 * (H - S) + 12.0;    // top edge of the black square, a little above the middle

    juce::String c;
    // black square, then the white cells of the data (merged per row, filled
    // as one path: no hairline seams between rows)
    c << "0 g\n" << rect (x0, yTop - S, S, S) << "1 g\n";
    bool anyWhite = false;
    for (int r = 1; r <= 8; ++r)
    {
        int cStart = -1;
        for (int col = 1; col <= 9; ++col)
        {
            const bool white = col <= 8 && ! cells[(size_t) (r * 10 + col)];
            if (white && cStart < 0) cStart = col;
            if (! white && cStart >= 0)
            {
                c << rectPath (x0 + (cStart - 1) * cell, yTop - r * cell, (col - cStart) * cell, cell);
                anyWhite = true;
                cStart = -1;
            }
        }
    }
    if (anyWhite) c << "f\n";
    c << "0 g\n";

    // labels
    const juce::String top = "TOP - this edge up, the printed side toward the bowl";
    c << text (0.5 * (W - textWidthMm (top, 10.0)), yTop + cell + 4.0, 10.0, top);
    const juce::String big = "id " + juce::String (id);
    c << text (0.5 * (W - textWidthMm (big, 28.0)), yTop - S - cell - 14.0, 28.0, big, true);
    const juce::String l1 = "AprilTag 36h11 - black square " + juce::String (S, 0) + " mm - " + title;
    c << text (0.5 * (W - textWidthMm (l1, 9.0)), yTop - S - cell - 21.0, 9.0, l1);
    const juce::String l2 = "Print at 100 % (actual size). The bar below must measure 100 mm.";
    c << text (0.5 * (W - textWidthMm (l2, 9.0)), yTop - S - cell - 26.0, 9.0, l2);

    // 100 mm scale bar with 10 mm ticks
    const double bx = 0.5 * (W - 100.0), by = yTop - S - cell - 36.0;
    c << rect (bx, by, 100.0, 0.6);
    for (int k = 0; k <= 10; ++k)
        c << rect (bx + 10.0 * k - 0.2, by, 0.4, k % 5 == 0 ? 4.0 : 2.5);
    c << text (bx - 2.0, by - 5.0, 8.0, "0");
    c << text (bx + 96.0, by - 5.0, 8.0, "100 mm");
    return c;
}
} // namespace

Paper paperFor (double tagSizeMm)
{
    const double needed = tagSizeMm * 10.0 / 8.0 + 12.0;   // the white border (1 cell a side) and the printer's margins
    if (needed <= 215.9) return { "Letter", 215.9, 279.4 };
    if (needed <= 279.4) return { "Tabloid", 279.4, 431.8 };
    return { "12 x 16 in", 304.8, 406.4 };   // up to a 234 mm square
}

bool markersPdf (const std::vector<int>& ids, double S, const Paper& paper, juce::MemoryBlock& out, const juce::String& title)
{
    out.reset();
    if (ids.empty() || ! (S >= 20.0) || S > paper.widthMm - 10.0) return false;
    for (int id : ids)
        if (id < 0 || id >= TagDetector::numIds()) return false;

    juce::MemoryOutputStream pdf (out, false);
    std::vector<int64_t> offsets;
    auto beginObject = [&] (int n)
    {
        if ((int) offsets.size() < n) offsets.resize ((size_t) n, 0);
        offsets[(size_t) (n - 1)] = (int64_t) pdf.getPosition();
        pdf << n << " 0 obj\n";
    };

    pdf << "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    const int nPages = (int) ids.size();
    const auto pageObj = [] (int i) { return 5 + 2 * i; };
    const auto contentObj = [] (int i) { return 6 + 2 * i; };

    beginObject (1);
    pdf << "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    beginObject (2);
    pdf << "<< /Type /Pages /Count " << nPages << " /Kids [";
    for (int i = 0; i < nPages; ++i) pdf << pageObj (i) << " 0 R ";
    pdf << "] >>\nendobj\n";
    beginObject (3);
    pdf << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";
    beginObject (4);
    pdf << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>\nendobj\n";

    const juce::String mediaBox = "[0 0 " + num (paper.widthMm * kPtPerMm) + " " + num (paper.heightMm * kPtPerMm) + "]";
    for (int i = 0; i < nPages; ++i)
    {
        beginObject (pageObj (i));
        pdf << "<< /Type /Page /Parent 2 0 R /MediaBox " << mediaBox
            << " /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " << contentObj (i) << " 0 R >>\nendobj\n";
        const auto content = pageContent (ids[(size_t) i], S, paper, title);
        const auto bytes = content.toStdString();
        beginObject (contentObj (i));
        pdf << "<< /Length " << (int) bytes.size() << " >>\nstream\n";
        pdf.write (bytes.data(), bytes.size());
        pdf << "endstream\nendobj\n";
    }

    const auto xref = pdf.getPosition();
    const int nObjects = (int) offsets.size();
    pdf << "xref\n0 " << (nObjects + 1) << "\n0000000000 65535 f \n";
    for (auto off : offsets)
        pdf << juce::String (off).paddedLeft ('0', 10) << " 00000 n \n";
    pdf << "trailer\n<< /Size " << (nObjects + 1) << " /Root 1 0 R >>\nstartxref\n" << (juce::int64) xref << "\n%%EOF\n";
    pdf.flush();
    return true;
}

bool writeMarkersPdf (const juce::File& file, const std::vector<int>& ids, double tagSizeMm)
{
    juce::MemoryBlock mb;
    if (! markersPdf (ids, tagSizeMm, paperFor (tagSizeMm), mb)) return false;
    return file.replaceWithData (mb.getData(), mb.getSize());
}
} // namespace scm::smap
