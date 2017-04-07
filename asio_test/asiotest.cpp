// asiotest.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <iostream>
#include <istream>
#include <ostream>
#include <string>
#include <functional>
#include "asio.hpp"
#include "use_task.hpp"
using asio::ip::tcp;

class client {
 public:
  client(asio::io_service& io_service,
         const std::string& server,
         const std::string& path)
      : resolver_(io_service), socket_(io_service) {
    // Form the request. We specify the "Connection: close" header so that the
    // server will close the socket after transmitting the response. This will
    // allow us to treat all data up until the EOF as the content.
    std::ostream request_stream(&request_);
    request_stream << "GET " << path << " HTTP/1.0\r\n";
    request_stream << "Host: " << server << "\r\n";
    request_stream << "Accept: */*\r\n";
    request_stream << "Connection: close\r\n\r\n";

    // Start an asynchronous resolve to translate the server and service names
    // into a list of endpoints.
    tcp::resolver::query query(server, "http");
    resolver_.async_resolve(
        query, std::bind(&client::handle_resolve, this, std::placeholders::_1,
                         std::placeholders::_2));
  }

 private:
  void handle_resolve(const asio::error_code& err,
                      tcp::resolver::iterator endpoint_iterator) {
    if (!err) {
      // Attempt a connection to each endpoint in the list until we
      // successfully establish a connection.
      asio::async_connect(
          socket_, endpoint_iterator,
          std::bind(&client::handle_connect, this, std::placeholders::_1));
    } else {
      std::cout << "Error: " << err.message() << "\n";
    }
  }

  void handle_connect(const asio::error_code& err) {
    if (!err) {
      // The connection was successful. Send the request.
      asio::async_write(socket_, request_,
                        std::bind(&client::handle_write_request, this,
                                  std::placeholders::_1));
    } else {
      std::cout << "Error: " << err.message() << "\n";
    }
  }

  void handle_write_request(const asio::error_code& err) {
    if (!err) {
      // Read the response status line. The response_ streambuf will
      // automatically grow to accommodate the entire line. The growth may be
      // limited by passing a maximum size to the streambuf constructor.
      asio::async_read_until(socket_, response_, "\r\n",
                             std::bind(&client::handle_read_status_line, this,
                                       std::placeholders::_1));
    } else {
      std::cout << "Error: " << err.message() << "\n";
    }
  }

  void handle_read_status_line(const asio::error_code& err) {
    if (!err) {
      // Check that response is OK.
      std::istream response_stream(&response_);
      std::string http_version;
      response_stream >> http_version;
      unsigned int status_code;
      response_stream >> status_code;
      std::string status_message;
      std::getline(response_stream, status_message);
      if (!response_stream || http_version.substr(0, 5) != "HTTP/") {
        std::cout << "Invalid response\n";
        return;
      }
      if (status_code != 200) {
        std::cout << "Response returned with status code ";
        std::cout << status_code << "\n";
        return;
      }

      // Read the response headers, which are terminated by a blank line.
      asio::async_read_until(
          socket_, response_, "\r\n\r\n",
          std::bind(&client::handle_read_headers, this, std::placeholders::_1));
    } else {
      std::cout << "Error: " << err << "\n";
    }
  }

  void handle_read_headers(const asio::error_code& err) {
    if (!err) {
      // Process the response headers.
      std::istream response_stream(&response_);
      std::string header;
      while (std::getline(response_stream, header) && header != "\r")
        std::cout << header << "\n";
      std::cout << "\n";

      // Write whatever content we already have to output.
      if (response_.size() > 0)
        std::cout << &response_;

      // Start reading remaining data until EOF.
      asio::async_read(
          socket_, response_, asio::transfer_at_least(1),
          std::bind(&client::handle_read_content, this, std::placeholders::_1));
    } else {
      std::cout << "Error: " << err << "\n";
    }
  }

  void handle_read_content(const asio::error_code& err) {
    if (!err) {
      // Write all of the data that has been read so far.
      std::cout << &response_;

      // Continue reading remaining data until EOF.
      asio::async_read(
          socket_, response_, asio::transfer_at_least(1),
          std::bind(&client::handle_read_content, this, std::placeholders::_1));
    } else if (err != asio::error::eof) {
      std::cout << "Error: " << err << "\n";
    }
  }

  tcp::resolver resolver_;
  tcp::socket socket_;
  asio::streambuf request_;
  asio::streambuf response_;
};

#if AWAITABLE_TASKS_EXCEPTION
awaitable_tasks::task<asio::error_code> make_http(asio::io_service& io_service,
                                                  const std::string& server,
                                                  const std::string& path) {
  asio::error_code err;
  try {
    asio::streambuf request_;
    std::ostream request_stream(&request_);
    request_stream << "GET " << path << " HTTP/1.0\r\n";
    request_stream << "Host: " << server << "\r\n";
    request_stream << "Accept: */*\r\n";
    request_stream << "Connection: close\r\n\r\n";

    // Start an asynchronous resolve to translate the server and service names
    // into a list of endpoints.
    tcp::resolver::query query(server, "http");
    tcp::resolver resolver_(io_service);
    tcp::socket socket_(io_service);
    auto resolver_ret = co_await resolver_.async_resolve(query, asio::use_task);

    // Attempt a connection to each endpoint in the list until we
    // successfully establish a connection.
    co_await asio::async_connect(socket_, resolver_ret, asio::use_task);

    co_await asio::async_write(socket_, request_, asio::use_task);

    asio::streambuf response_;
    co_await asio::async_read_until(socket_, response_, "\r\n", asio::use_task);

    // Check that response is OK.
    std::istream response_stream(&response_);
    std::string http_version;
    response_stream >> http_version;
    unsigned int status_code;
    response_stream >> status_code;
    std::string status_message;
    std::getline(response_stream, status_message);
    if (!response_stream || http_version.substr(0, 5) != "HTTP/") {
      std::cout << "Invalid response\n";
      return asio::error_code();
    }
    if (status_code != 200) {
      std::cout << "Response returned with status code ";
      std::cout << status_code << "\n";
      return asio::error_code();
    }

    // Read the response headers, which are terminated by a blank line.
    co_await asio::async_read_until(socket_, response_, "\r\n\r\n",
                                    asio::use_task);

    // Process the response headers.
    std::istream response_stream2(&response_);
    std::string header;
    while (std::getline(response_stream2, header) && header != "\r")
      std::cout << header << "\n";
    std::cout << "\n";

    // Write whatever content we already have to output.
    if (response_.size() > 0)
      std::cout << &response_;

    // Continue reading remaining data until EOF.
    for (;;) {
      auto count = co_await asio::async_read(
          socket_, response_, asio::transfer_at_least(1), asio::use_task);
      if (count) {
        std::cout << &response_;
        continue;
      } else {
        break;
      }
    }
  } catch (std::system_error& e) {
    if (e.code() != asio::error::eof) {
      std::cout << "Exception: " << e.what() << "\n";
      return asio::error_code(e.code());
    }
  }
  return asio::error_code();
}

#else

awaitable_tasks::task<asio::error_code> make_http(asio::io_service& io_service,
                                                  const std::string& server,
                                                  const std::string& path) {
  asio::error_code err;

  asio::streambuf request_;
  std::ostream request_stream(&request_);
  request_stream << "GET " << path << " HTTP/1.0\r\n";
  request_stream << "Host: " << server << "\r\n";
  request_stream << "Accept: */*\r\n";
  request_stream << "Connection: close\r\n\r\n";
  tcp::socket socket_(io_service);
  tcp::resolver resolver_(io_service);
  tcp::resolver::query query(server, "http");

  // Start an asynchronous resolve to translate the server and service names
  // into a list of endpoints.
  auto t1 = resolver_.async_resolve(query, asio::use_task);
  auto resolver_ret = co_await t1;

  err = std::get<0>(resolver_ret);
  if (err)
    return err;

  // Attempt a connection to each endpoint in the list until we
  // successfully establish a connection.
  auto&& connect_ret = co_await asio::async_connect(
      socket_, std::get<1>(resolver_ret), asio::use_task);
  err = std::get<0>(connect_ret);
  if (err)
    return err;

  auto&& write_ret =
      co_await asio::async_write(socket_, request_, asio::use_task);
  err = std::get<0>(write_ret);
  if (err)
    return err;

  asio::streambuf response_;
  auto&& read_ret = co_await asio::async_read_until(socket_, response_, "\r\n",
                                                    asio::use_task);
  err = std::get<0>(read_ret);
  if (err)
    return err;

  // Check that response is OK.
  std::istream response_stream(&response_);
  std::string http_version;
  response_stream >> http_version;
  unsigned int status_code;
  response_stream >> status_code;
  std::string status_message;
  std::getline(response_stream, status_message);
  if (!response_stream || http_version.substr(0, 5) != "HTTP/") {
    std::cout << "Invalid response\n";
    return asio::error_code();
  }
  if (status_code != 200) {
    std::cout << "Response returned with status code ";
    std::cout << status_code << "\n";
    return asio::error_code();
  }

  // Read the response headers, which are terminated by a blank line.
  auto&& read_ret2 = co_await asio::async_read_until(
      socket_, response_, "\r\n\r\n", asio::use_task);

  // Process the response headers.
  std::istream response_stream2(&response_);
  std::string header;
  while (std::getline(response_stream2, header) && header != "\r")
    std::cout << header << "\n";
  std::cout << "\n";

  // Write whatever content we already have to output.
  if (response_.size() > 0)
    std::cout << &response_;

  // Continue reading remaining data until EOF.
  for (;;) {
    auto&& rett = co_await asio::async_read(
        socket_, response_, asio::transfer_at_least(1), asio::use_task);
    err = std::get<0>(rett);
    if (!err) {
      std::cout << &response_;
      continue;
    } else {
      if (err == asio::error::eof)
        err = asio::error_code();
      break;
    }
  }
  return err;
}
#endif

int main(int argc, char* argv[]) {
  try {
    auto path = "www.boost.org";
    auto server = "/LICENSE_1_0.txt";
    if (argc == 3) {
      path = argv[1];
      server = argv[2];
    }
    //{
    // 			asio::io_service io_service;
    //
    // 			client c(io_service, path, server);
    // 			io_service.run();
    //)
    {
      asio::io_service io_service;
      auto t = make_http(io_service, path, server);
	  t.reset();
      io_service.run();
    }

  } catch (std::exception& e) {
    std::cout << "Exception: " << e.what() << "\n";
  }

  return 0;
}
