#include "../protocol/jsonl.h"
#include "../engine.h"
namespace mvd::protocol {
int serve(Engine& engine,std::istream& input,std::ostream& output,bool default_ann,const volatile std::sig_atomic_t& stop){
 auto emit_json_response=[&](const Response& response,const std::string& id){output<<encode_response(response,id);output.flush();};
 auto emit_error=[&](const std::string& id,const std::string& code,const std::string& message){output<<encode_error(id,code,message);output.flush();};
    engine.open();
    if (default_ann) engine.set_ann_mode(true);

    // The judge waits for this exact object before sending any request.
    output << "{\"type\":\"ready\"}\n";
    output.flush();

    std::string line;
    while (!stop && std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;  // no response is defined for blank lines

      Request req;
      std::string parse_error;
      if (!parse_request(line, req, parse_error)) {
        emit_error("null", "invalid_request", parse_error);
        continue;
      }
      if (req.op == "shutdown") {
        engine.shutdown();
        std::string out = "{\"id\":" + req.id_raw + ",\"ok\":true}\n";
        output << out;
        output.flush();
        return 0;
      }
      if (req.op != "execute") {
        emit_error(req.id_raw, "unsupported_op", "unsupported operation");
        continue;
      }
      // Each execute starts from its configured default (exact for the
      // standard entrypoint), rather than inheriting a previous request.
      engine.set_ann_mode(req.search_mode.empty() ? default_ann :
                          req.search_mode == "ann" || req.search_mode == "approximate");

      mvd::Response resp = engine.execute(req.sql);
      emit_json_response(resp, req.id_raw);
    }

    engine.shutdown();
    return 0;
}
}
