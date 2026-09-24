#include "Visual/svgCanvas.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <atomic>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    std::atomic<int> resourceRequests = 0;
    httplib::Server resources;
    resources.Get("/.*", [&](const auto&, auto& response)
    { ++resourceRequests; response.set_content("unexpected resource request", "text/plain"); });
    const int port = resources.bind_to_any_port("127.0.0.1");
    if (port <= 0) return 1;
    std::jthread listener([&] { resources.listen_after_bind(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!resources.is_running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!resources.is_running()) { resources.stop(); listener.join(); return 1; }

    const std::string remote = "http://127.0.0.1:" + std::to_string(port) + "/paint.svg#p";
    bool passed = true;
    for (const std::string attack : {
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect fill=\"u&#114;l(" + remote + ")\"/></svg>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:h=\"http://www.w3.org/1999/xlink\"><use h:href=\"" + remote + "\"/></svg>"})
        passed = passed && !revia::visual::SvgSanitizer::Sanitize(attack).accepted;

    const auto safe = revia::visual::SvgSanitizer::Sanitize(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 20"><defs><linearGradient id="p"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#ff0000"/></linearGradient></defs><rect width="20" height="20" fill="u&#114;l(#p)"/></svg>)SVG");
    passed = passed && safe.accepted;
    QSvgRenderer renderer(QByteArray::fromStdString(safe.markup));
    passed = passed && renderer.isValid();
    QImage rendered(20, 20, QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    renderer.render(&painter);
    painter.end();
    application.processEvents();
    passed = passed && rendered.pixelColor(10, 10) == QColor(Qt::red);
    passed = passed && resourceRequests.load() == 0;
    resources.stop();
    listener.join();
    std::cout << (passed ? "Validated SVG renders locally without resource requests.\n" : "SVG renderer regression failed.\n");
    return passed ? 0 : 1;
}
