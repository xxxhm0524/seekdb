#include "engine.h"
#include "runtime/database.h"
namespace mvd {
Engine::Engine(std::string path,size_t pages,size_t memory,size_t ann_rows,size_t sort_memory)
 :database_(std::make_unique<detail::Database>(std::move(path),pages,memory,ann_rows,sort_memory)){}
Engine::~Engine()=default;
void Engine::open(){database_->open();}
void Engine::shutdown(){database_->shutdown();}
Response Engine::execute(const std::string& sql){return database_->execute(sql);}
bool Engine::ann_mode()const{return database_->ann_mode();}
void Engine::set_ann_mode(bool value){database_->set_ann_mode(value);}
bool Engine::in_transaction()const{return database_->in_transaction();}
}
