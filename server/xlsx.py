"""Мини-генератор XLSX на чистой стандартной библиотеке.

Нужен ровно для одного: отдать владельцу «красивую» книгу с таблицами,
ссылками между листами и диаграммами. Без openpyxl и прочих зависимостей.

Поддерживает: несколько листов, стили из фиксированного набора, ширины
колонок, закрепление шапки, внутренние гиперссылки, линейные и столбчатые
диаграммы с подписями и кэшем значений.
"""

import zipfile
from io import BytesIO
from xml.sax.saxutils import escape

# ---------------------------------------------------------------- стили
# Номера в cellXfs ниже; расширять только вместе со styles_xml().
S_DEFAULT = 0
S_BOLD = 1
S_TITLE = 2
S_HEADER = 3
S_NUM2 = 4      # 0.00
S_NUM1 = 5      # 0.0
S_INT = 6       # 0
S_DT = 7        # дата и время
S_LINK = 8
S_MUTED = 9
S_NUM3 = 10     # 0.000
S_HHMM = 11
S_SECTION = 12
S_TABLE_HEAD = 13
S_CENTER = 14

NUMFMTS = [
    (164, "0.0"),
    (165, "0"),
    (166, "dd.mm.yyyy hh:mm"),
    (167, "0.000"),
    (168, "hh:mm"),
    (169, "0.00"),
]


def styles_xml():
    numfmts = "".join(
        '<numFmt numFmtId="%d" formatCode="%s"/>' % (i, escape(f, {'"': "&quot;"}))
        for i, f in NUMFMTS
    )
    fonts = (
        '<font><sz val="11"/><color rgb="FF22302A"/><name val="Calibri"/><family val="2"/></font>'
        '<font><b/><sz val="11"/><color rgb="FF22302A"/><name val="Calibri"/><family val="2"/></font>'
        '<font><b/><sz val="15"/><color rgb="FF1F3A2E"/><name val="Calibri"/><family val="2"/></font>'
        '<font><b/><sz val="11"/><color rgb="FFFFFFFF"/><name val="Calibri"/><family val="2"/></font>'
        '<font><u/><sz val="11"/><color rgb="FF1565C0"/><name val="Calibri"/><family val="2"/></font>'
        '<font><sz val="10"/><color rgb="FF6B7B72"/><name val="Calibri"/><family val="2"/></font>'
    )
    fills = (
        '<fill><patternFill patternType="none"/></fill>'
        '<fill><patternFill patternType="gray125"/></fill>'
        '<fill><patternFill patternType="solid"><fgColor rgb="FF1F7A4D"/><bgColor indexed="64"/></patternFill></fill>'
        '<fill><patternFill patternType="solid"><fgColor rgb="FFEDF4F0"/><bgColor indexed="64"/></patternFill></fill>'
    )
    border = ('<border><left/><right/><top/><bottom/><diagonal/></border>'
              '<border><left style="thin"><color rgb="FFD6E2DB"/></left>'
              '<right style="thin"><color rgb="FFD6E2DB"/></right>'
              '<top style="thin"><color rgb="FFD6E2DB"/></top>'
              '<bottom style="thin"><color rgb="FFD6E2DB"/></bottom><diagonal/></border>')
    xfs = (
        '<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>'
        '<xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/>'
        '<xf numFmtId="0" fontId="2" fillId="0" borderId="0" xfId="0" applyFont="1"/>'
        '<xf numFmtId="0" fontId="3" fillId="2" borderId="0" xfId="0" applyFont="1"'
        ' applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>'
        '<xf numFmtId="169" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>'
        '<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>'
        '<xf numFmtId="165" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>'
        '<xf numFmtId="166" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>'
        '<xf numFmtId="0" fontId="4" fillId="0" borderId="0" xfId="0" applyFont="1"/>'
        '<xf numFmtId="0" fontId="5" fillId="0" borderId="0" xfId="0" applyFont="1"/>'
        '<xf numFmtId="167" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>'
        '<xf numFmtId="168" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>'
        '<xf numFmtId="0" fontId="1" fillId="3" borderId="1" xfId="0" applyFont="1"'
        ' applyFill="1" applyBorder="1"/>'
        '<xf numFmtId="0" fontId="1" fillId="0" borderId="1" xfId="0" applyFont="1"'
        ' applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>'
        '<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0" applyAlignment="1">'
        '<alignment horizontal="center"/></xf>'
    )
    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
        '<numFmts count="%d">%s</numFmts>'
        '<fonts count="6">%s</fonts>'
        '<fills count="4">%s</fills>'
        '<borders count="2">%s</borders>'
        '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
        '<cellXfs count="%d">%s</cellXfs>'
        '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>'
        '</styleSheet>'
    ) % (len(NUMFMTS), numfmts, fonts, fills, border, len(xfs), xfs)


# ---------------------------------------------------------------- ячейки

class Cell:
    __slots__ = ("value", "style", "link")

    def __init__(self, value=None, style=S_DEFAULT, link=None):
        self.value = value
        self.style = style
        self.link = link  # dict(location=..., display=...)


def _num(v):
    if isinstance(v, float):
        if v != v or v in (float("inf"), float("-inf")):
            return "0"
        return repr(round(v, 6))
    return str(v)


def _col_letter(col: int) -> str:
    out = ""
    while col > 0:
        col, rem = divmod(col - 1, 26)
        out = chr(65 + rem) + out
    return out


def _cell_xml(ref: str, cell: "Cell") -> str:
    v = cell.value
    style = ' s="%d"' % cell.style if cell.style else ""
    if v is None:
        return '<c r="%s"%s/>' % (ref, style) if cell.style else ""
    if isinstance(v, bool):
        return '<c r="%s"%s t="b"><v>%d</v></c>' % (ref, style, 1 if v else 0)
    if isinstance(v, (int, float)):
        return '<c r="%s"%s><v>%s</v></c>' % (ref, style, _num(v))
    text = escape(str(v))
    return ('<c r="%s"%s t="inlineStr"><is><t xml:space="preserve">%s</t></is></c>'
            % (ref, style, text))


class Series:
    def __init__(self, name, cats, values, ref_name, ref_val, color="1F7A4D", kind="line"):
        self.name = name
        self.cats = cats
        self.values = values
        self.ref_name = ref_name
        self.ref_val = ref_val
        self.color = color
        self.kind = kind


class Chart:
    def __init__(self, title, anchor, cats_ref, series, y_title=None, kind="line"):
        self.title = title
        self.anchor = anchor          # (from_col, from_row, to_col, to_row) 0-based
        self.cats_ref = cats_ref
        self.series = series
        self.y_title = y_title
        self.kind = kind


class Sheet:
    def __init__(self, name):
        self.name = name
        self.rows = {}          # row -> {col: Cell}
        self.widths = {}        # col -> width
        self.freeze_rows = 0
        self.charts = []

    def set(self, row: int, col: int, value=None, style=S_DEFAULT, link=None):
        self.rows.setdefault(row, {})[col] = Cell(value, style, link)

    def link(self, row: int, col: int, text: str, location: str):
        self.set(row, col, text, S_LINK, {"location": location, "display": text})

    def add_chart(self, chart: "Chart"):
        self.charts.append(chart)


# ---------------------------------------------------------------- диаграммы

def _str_cache(values):
    pts = "".join('<c:pt idx="%d"><c:v>%s</c:v></c:pt>' % (i, escape(str(v)))
                  for i, v in enumerate(values) if v is not None)
    return '<c:strCache><c:ptCount val="%d"/>%s</c:strCache>' % (len(values), pts)


def _num_cache(values):
    pts = ""
    for i, v in enumerate(values):
        if v is None:
            continue
        pts += '<c:pt idx="%d"><c:v>%s</c:v></c:pt>' % (i, _num(float(v)))
    return ('<c:numCache><c:formatCode>General</c:formatCode><c:ptCount val="%d"/>%s</c:numCache>'
            % (len(values), pts))


def _series_xml(sr: "Series", idx: int, cats_ref: str) -> str:
    if sr.kind == "bar":
        # CT_BarSer: idx, order, tx, spPr, cat, val (без marker/smooth)
        extra = ('<c:spPr><a:solidFill><a:srgbClr val="%s"/></a:solidFill>'
                 '<a:ln><a:noFill/></a:ln></c:spPr>' % sr.color)
        tail = ""
    else:
        # CT_LineSer: idx, order, tx, spPr, marker, cat, val, smooth
        extra = ('<c:spPr><a:ln w="22000"><a:solidFill><a:srgbClr val="%s"/></a:solidFill>'
                 '<a:round/></a:ln></c:spPr>'
                 '<c:marker><c:symbol val="circle"/><c:size val="4"/></c:marker>' % sr.color)
        tail = '<c:smooth val="1"/>'
    return (
        '<c:ser><c:idx val="%d"/><c:order val="%d"/>'
        '<c:tx><c:strRef><c:f>%s</c:f>%s</c:strRef></c:tx>'
        '%s'
        '<c:cat><c:strRef><c:f>%s</c:f>%s</c:strRef></c:cat>'
        '<c:val><c:numRef><c:f>%s</c:f>%s</c:numRef></c:val>'
        '%s</c:ser>'
    ) % (idx, idx, escape(sr.ref_name), _str_cache([sr.name]), extra,
         escape(cats_ref), _str_cache(sr.cats), escape(sr.ref_val), _num_cache(sr.values), tail)


def chart_xml(ch: "Chart") -> str:
    is_bar = ch.kind == "bar"
    plot = ('<c:barChart><c:barDir val="col"/><c:grouping val="clustered"/>'
            '<c:varyColors val="0"/>'
            + "".join(_series_xml(sr, i, ch.cats_ref) for i, sr in enumerate(ch.series))
            + '<c:gapWidth val="60"/><c:axId val="50010001"/><c:axId val="50020002"/>'
              '</c:barChart>') if is_bar else (
        '<c:lineChart><c:grouping val="standard"/><c:varyColors val="0"/>'
        + "".join(_series_xml(sr, i, ch.cats_ref) for i, sr in enumerate(ch.series))
        + '<c:marker val="1"/><c:axId val="50010001"/><c:axId val="50020002"/></c:lineChart>')

    y_title = ('<c:title><c:tx><c:rich><a:bodyPr/><a:p><a:pPr><a:defRPr sz="900"/></a:pPr>'
               '<a:r><a:rPr sz="900"/><a:t>%s</a:t></a:r></a:p></c:rich></c:tx>'
               '<c:overlay val="0"/></c:title>' % escape(ch.y_title)) if ch.y_title else ""

    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart"'
        ' xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"'
        ' xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
        '<c:chart>'
        '<c:title><c:tx><c:rich><a:bodyPr/><a:p><a:pPr><a:defRPr sz="1100" b="1"/></a:pPr>'
        '<a:r><a:rPr sz="1100" b="1"/><a:t>%s</a:t></a:r></a:p></c:rich></c:tx>'
        '<c:overlay val="0"/></c:title><c:autoTitleDeleted val="0"/>'
        '<c:plotArea><c:layout/>%s'
        '<c:catAx><c:axId val="50010001"/><c:scaling><c:orientation val="minMax"/></c:scaling>'
        '<c:delete val="0"/><c:axPos val="b"/><c:numFmt formatCode="General" sourceLinked="1"/>'
        '<c:majorTickMark val="none"/><c:minorTickMark val="none"/><c:tickLblPos val="nextTo"/>'
        '<c:crossAx val="50020002"/><c:crosses val="autoZero"/><c:auto val="1"/>'
        '<c:lblAlgn val="ctr"/><c:lblOffset val="100"/></c:catAx>'
        '<c:valAx><c:axId val="50020002"/><c:scaling><c:orientation val="minMax"/></c:scaling>'
        '<c:delete val="0"/><c:axPos val="l"/><c:majorGridlines/>%s'
        '<c:numFmt formatCode="General" sourceLinked="1"/>'
        '<c:majorTickMark val="none"/><c:minorTickMark val="none"/><c:tickLblPos val="nextTo"/>'
        '<c:crossAx val="50010001"/><c:crosses val="autoZero"/><c:crossBetween val="between"/>'
        '</c:valAx></c:plotArea>'
        '<c:legend><c:legendPos val="b"/><c:overlay val="0"/></c:legend>'
        '<c:plotVisOnly val="1"/><c:dispBlanksAs val="gap"/>'
        '</c:chart></c:chartSpace>'
    ) % (escape(ch.title), plot, y_title)


def drawing_xml(charts, rel_start: int) -> str:
    anchors = []
    for i, ch in enumerate(charts):
        c0, r0, c1, r1 = ch.anchor
        anchors.append(
            '<xdr:twoCellAnchor>'
            '<xdr:from><xdr:col>%d</xdr:col><xdr:colOff>0</xdr:colOff>'
            '<xdr:row>%d</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from>'
            '<xdr:to><xdr:col>%d</xdr:col><xdr:colOff>0</xdr:colOff>'
            '<xdr:row>%d</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:to>'
            '<xdr:graphicFrame macro="">'
            '<xdr:nvGraphicFramePr><xdr:cNvPr id="%d" name="Диаграмма %d"/>'
            '<xdr:cNvGraphicFramePr/></xdr:nvGraphicFramePr>'
            '<xdr:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/></xdr:xfrm>'
            '<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart">'
            '<c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart"'
            ' xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'
            ' r:id="rId%d"/></a:graphicData></a:graphic></xdr:graphicFrame>'
            '<xdr:clientData/></xdr:twoCellAnchor>'
            % (c0, r0, c1, r1, i + 2, i + 1, rel_start + i)
        )
    return ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<xdr:wsDr xmlns:xdr="http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing"'
            ' xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
            + "".join(anchors) + '</xdr:wsDr>')


# ---------------------------------------------------------------- книга

class Workbook:
    def __init__(self):
        self.sheets = []

    def add(self, name: str) -> Sheet:
        base = "".join(c for c in name if c not in ':\\/?*[]').strip()[:28] or "Лист"
        final, i = base, 2
        used = {s.name for s in self.sheets}
        while final in used:
            suffix = " %d" % i
            final = base[:28 - len(suffix)] + suffix
            i += 1
        sh = Sheet(final)
        self.sheets.append(sh)
        return sh

    def sheet_names(self):
        return [s.name for s in self.sheets]

    def save(self) -> bytes:
        # нумерация листов/рисунков/диаграмм
        chart_id = 0
        drawing_id = 0
        sheet_drawings = {}
        chart_files = {}
        drawing_files = {}
        for idx, sh in enumerate(self.sheets, start=1):
            if not sh.charts:
                continue
            drawing_id += 1
            drawing_files[idx] = drawing_id
            rels = []
            for ch in sh.charts:
                chart_id += 1
                chart_files.setdefault(idx, []).append(chart_id)
                rels.append(chart_id)
            sheet_drawings[idx] = rels

        parts = {}

        # ---------- листы
        for idx, sh in enumerate(self.sheets, start=1):
            parts["xl/worksheets/sheet%d.xml" % idx] = self._sheet_xml(sh, idx in sheet_drawings)
            if idx in sheet_drawings:
                parts["xl/worksheets/_rels/sheet%d.xml.rels" % idx] = (
                    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                    '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                    '<Relationship Id="rId1" '
                    'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/drawing" '
                    'Target="../drawings/drawing%d.xml"/></Relationships>' % drawing_files[idx])

        # ---------- рисунки и диаграммы
        for idx, chart_ids in sheet_drawings.items():
            sh = self.sheets[idx - 1]
            rel_parts = []
            for i, cid in enumerate(chart_ids, start=1):
                rel_parts.append(
                    '<Relationship Id="rId%d" '
                    'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart" '
                    'Target="../charts/chart%d.xml"/>' % (i, cid))
            parts["xl/drawings/_rels/drawing%d.xml.rels" % drawing_files[idx]] = (
                '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                + "".join(rel_parts) + '</Relationships>')
            parts["xl/drawings/drawing%d.xml" % drawing_files[idx]] = drawing_xml(sh.charts, 1)
            for cid, ch in zip(chart_ids, sh.charts):
                parts["xl/charts/chart%d.xml" % cid] = chart_xml(ch)

        # ---------- общие части
        parts["xl/workbook.xml"] = (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"'
            ' xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
            '<sheets>'
            + "".join('<sheet name="%s" sheetId="%d" r:id="rId%d"/>'
                      % (escape(sh.name, {'"': "&quot;"}), i, i)
                      for i, sh in enumerate(self.sheets, start=1))
            + '</sheets></workbook>')

        styles_rid = len(self.sheets) + 1
        parts["xl/_rels/workbook.xml.rels"] = (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            + "".join('<Relationship Id="rId%d" '
                      'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" '
                      'Target="worksheets/sheet%d.xml"/>' % (i, i)
                      for i in range(1, len(self.sheets) + 1))
            + '<Relationship Id="rId%d" '
              'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" '
              'Target="styles.xml"/>' % styles_rid
            + '</Relationships>')

        parts["xl/styles.xml"] = styles_xml()
        parts["_rels/.rels"] = (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" '
            'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" '
            'Target="xl/workbook.xml"/></Relationships>')

        overrides = ["".join(
            '<Override PartName="/xl/worksheets/sheet%d.xml" '
            'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>' % i
            for i in range(1, len(self.sheets) + 1))]
        overrides.append('<Override PartName="/xl/styles.xml" '
                         'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>')
        for d in drawing_files.values():
            overrides.append('<Override PartName="/xl/drawings/drawing%d.xml" '
                             'ContentType="application/vnd.openxmlformats-officedocument.drawing+xml"/>' % d)
        for c in range(1, chart_id + 1):
            overrides.append('<Override PartName="/xl/charts/chart%d.xml" '
                             'ContentType="application/vnd.openxmlformats-officedocument.drawingml.chart+xml"/>' % c)
        parts["[Content_Types].xml"] = (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
            '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
            '<Default Extension="xml" ContentType="application/xml"/>'
            '<Override PartName="/xl/workbook.xml" '
            'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
            + "".join(overrides) + '</Types>')

        buf = BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
            for name, data in parts.items():
                z.writestr(name, data.encode("utf-8"))
        return buf.getvalue()

    def _sheet_xml(self, sh: Sheet, has_drawing: bool) -> str:
        max_row = max(sh.rows) if sh.rows else 1
        max_col = max((max(r) for r in sh.rows.values()), default=1)
        rows_xml = []
        for r in sorted(sh.rows):
            cells = "".join(_cell_xml("%s%d" % (_col_letter(c), r), cell)
                            for c, cell in sorted(sh.rows[r].items()))
            rows_xml.append('<row r="%d"%s>%s</row>'
                            % (r, ' ht="18" customHeight="1"' if r == 1 else "", cells))

        cols_xml = ""
        if sh.widths:
            cols_xml = "<cols>" + "".join(
                '<col min="%d" max="%d" width="%.2f" customWidth="1"/>' % (c, c, w)
                for c, w in sorted(sh.widths.items())) + "</cols>"

        links = []
        for r, cells in sh.rows.items():
            for c, cell in cells.items():
                if cell.link:
                    links.append('<hyperlink ref="%s%d" location="%s" display="%s"/>'
                                 % (_col_letter(c), r,
                                    escape(cell.link["location"], {'"': "&quot;"}),
                                    escape(cell.link.get("display", ""), {'"': "&quot;"})))
        links_xml = '<hyperlinks>%s</hyperlinks>' % "".join(links) if links else ""

        pane = ""
        if sh.freeze_rows:
            pane = ('<pane ySplit="%d" topLeftCell="A%d" activePane="bottomLeft" state="frozen"/>'
                    % (sh.freeze_rows, sh.freeze_rows + 1))

        return (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"'
            ' xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
            '<dimension ref="A1:%s%d"/>'
            '<sheetViews><sheetView workbookViewId="0">%s</sheetView></sheetViews>'
            '<sheetFormatPr defaultRowHeight="15"/>%s<sheetData>%s</sheetData>%s'
            '<pageMargins left="0.6" right="0.6" top="0.6" bottom="0.6" header="0.3" footer="0.3"/>'
            '%s</worksheet>'
        ) % (_col_letter(max_col), max_row, pane, cols_xml, "".join(rows_xml), links_xml,
             '<drawing r:id="rId1"/>' if has_drawing else "")
