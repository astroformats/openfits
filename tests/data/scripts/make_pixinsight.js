// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz
//
// Writes samples E1 and E2 with PixInsight: the 37 x 23 pattern image, created with PixelMath as Gray UInt16, with
// keywords of its own and a HISTORY record, saved as FITS of 16-bit unsigned integers; and its three channels as an RGB
// image of 32-bit floating point, saved as a FITS cube of NAXIS3 = 3. The FITS module of PixInsight 1.9.5 writes no
// ICCProfile unit in automation mode, whatever profile the image has and the hint icc-profile; its XISF files embed
// one. tests/samples/sample_catalog.cpp describes the samples. Run it in automation mode, for example on Windows:
//
//   PixInsight.exe -n --automation-mode -r="make_pixinsight.js,<output directory>" --force-exit
//
// It writes a log, pixinsight.log, next to the sample.

var outputDirectory = (typeof jsArguments !== "undefined" && jsArguments.length > 0) ?
    jsArguments[0] : File.extractDirectory(#__FILE__);

var log = "";

function note(text)
{
    log += text + "\n";
}

var hints = "unsigned bits-per-sample 16 fits-keywords properties";
var floatHints = "floating-point bits-per-sample 32 fits-keywords properties";

function writeSample()
{
    var id = "openfits_sample_e1";
    var P = new PixelMath;
    P.expression = "(x() + 37*y())/850";
    P.useSingleExpression = true;
    P.rescale = false;
    P.truncate = true;
    P.createNewImage = true;
    P.showNewImage = true;
    P.newImageId = id;
    P.newImageWidth = 37;
    P.newImageHeight = 23;
    P.newImageAlpha = false;
    P.newImageColorSpace = PixelMath.prototype.Gray;
    P.newImageSampleFormat = PixelMath.prototype.i16;
    if (!P.executeGlobal())
        throw new Error("PixelMath failed");

    var window = ImageWindow.windowById(id);
    if (window.isNull)
        throw new Error("PixelMath created no image");
    var keywords = window.keywords;
    keywords.push(new FITSKeyword("OBJECT", "'pattern'", "The pattern image of the samples"));
    keywords.push(new FITSKeyword("EXPTIME", "300.", "Exposure time in seconds"));
    keywords.push(new FITSKeyword("HISTORY", "", "Written by the OpenFITS sample script"));
    window.keywords = keywords;
    var path = outputDirectory + "/e1-pixinsight-u16.fits";
    if (!window.saveAs(path, false, false, false, false, hints))
        throw new Error("cannot save " + path);
    window.forceClose();
    note("wrote " + path);
}

function writeColourSample()
{
    var id = "openfits_sample_e2";
    var P = new PixelMath;
    P.expression = "(x() + 37*y())/850";
    P.expression1 = "1 - (x() + 37*y())/850";
    P.expression2 = "x()/36";
    P.useSingleExpression = false;
    P.rescale = false;
    P.truncate = true;
    P.createNewImage = true;
    P.showNewImage = true;
    P.newImageId = id;
    P.newImageWidth = 37;
    P.newImageHeight = 23;
    P.newImageAlpha = false;
    P.newImageColorSpace = PixelMath.prototype.RGB;
    P.newImageSampleFormat = PixelMath.prototype.f32;
    if (!P.executeGlobal())
        throw new Error("PixelMath failed");

    var window = ImageWindow.windowById(id);
    if (window.isNull)
        throw new Error("PixelMath created no image");
    var keywords = window.keywords;
    keywords.push(new FITSKeyword("OBJECT", "'pattern'", "The three channels of the pattern image"));
    window.keywords = keywords;
    var path = outputDirectory + "/e2-pixinsight-rgb-f32.fits";
    if (!window.saveAs(path, false, false, false, false, floatHints))
        throw new Error("cannot save " + path);
    window.forceClose();
    note("wrote " + path);
}

try
{
    if (typeof coreVersionMajor !== "undefined")
        note("PixInsight " + coreVersionMajor + "." + coreVersionMinor + "." + coreVersionRelease + "-" +
             coreVersionRevision + " build " + coreVersionBuild);
    note("format hints: " + hints + "; " + floatHints);
    writeSample();
    writeColourSample();
    note("done");
}
catch (failure)
{
    note("error: " + failure);
}

File.writeTextFile(outputDirectory + "/pixinsight.log", log);
