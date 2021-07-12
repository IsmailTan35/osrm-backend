.onAttach <- function(libname, pkgname) {
  packageStartupMessage("Data: (c) OpenStreetMap contributors, ODbL 1.0 - http://www.openstreetmap.org/copyright")
  packageStartupMessage("Routing: OSRM - http://project-osrm.org/")
  # options(osrm.server = "http://0.0.0.0:9000/", osrm.profile = "driving")
}

.onLoad <- function(libname, pkgname) {
  options(osrm.server = "https://routing.openstreetmap.de/", osrm.profile = "car")
  # options(osrm.server = "http://0.0.0.0:9000/", osrm.profile = "driving")
}