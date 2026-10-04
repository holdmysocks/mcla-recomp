/* Link-time stub naming an import from the console's libSceSystemService.
 *
 * The payload SDK's stub for that library does not list
 * sceSystemServiceHideSplashScreen, so the title converter refuses a title
 * that imports it. ps5/title_build.sh builds this into a stub shared object,
 * exactly as the driver project does for its libSceAgc imports; the body is
 * never run, the real function comes from the system module at load.
 */

int sceSystemServiceHideSplashScreen(void) { return -1; }
