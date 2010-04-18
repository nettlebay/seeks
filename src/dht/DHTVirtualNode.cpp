/**
 * This is the p2p messaging component of the Seeks project,
 * a collaborative websearch overlay network.
 *
 * Copyright (C) 2006, 2010  Emmanuel Benazera, juban@free.fr
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "DHTVirtualNode.h"
#include "DHTNode.h"
#include "FingerTable.h"
#include "RouteIterator.h"
#include "errlog.h"

using sp::errlog;

namespace dht
{
   DHTVirtualNode::DHTVirtualNode(DHTNode* pnode)
     : _pnode(pnode),_successor(NULL),_predecessor(NULL),
       _successors(this)
       {
	  /**
	   * We generate a random key as the node's id.
	   */
	  _idkey = DHTKey::randomKey();
	  //_idkey = DHTNode::generate_uniform_key();
	  
	  /**
	   * create location table. TODO: read from serialized table.
	   */
	  _lt = new LocationTable();
	  
	  /**
	   * create location and registers it to the location table.
	   */
	  Location *lloc = NULL;
	  addToLocationTable(_idkey, getNetAddress(), lloc);
	  
	  /**
	   * finger table.
	   */
	  _fgt = new FingerTable(this,_lt);
       
	  /**
	   * Initializes mutexes.
	   */
	  seeks_proxy::mutex_init(&_pred_mutex);
	  seeks_proxy::mutex_init(&_succ_mutex);
       }

   DHTVirtualNode::~DHTVirtualNode()
     {
	if (_successor)
	  delete _successor;
	if (_predecessor)
	  delete _predecessor;
	
	// TODO: deregister succlist from stabilizer !!
	_successors.clear();
	
	delete _fgt;
     }
      
   dht_err DHTVirtualNode::notify(const DHTKey& senderKey, const NetAddress& senderAddress)
     {
	bool reset_pred = false;
	if (!_predecessor)
	  reset_pred = true;
	else
	  {
	     /**
	      * If we are the new successor of a node, because some other node did 
	      * fail in between us, then we need to make sure that this dead node is
	      * not our predecessor. If it is, then we do accept the change.
	      */
	     // TODO: slow down the time between two pings by looking at a location 'alive' flag.
	     Location *pred_loc = findLocation(*_predecessor);
	     int status = 0;
	     bool dead = is_dead(pred_loc->getDHTKey(),pred_loc->getNetAddress(),status);
	     if (dead)
	       reset_pred = true;
	     else if (senderKey.between(*_predecessor, _idkey))
	       reset_pred = true;
	  }
	
	if (reset_pred)
	  setPredecessor(senderKey, senderAddress);
	return DHT_ERR_OK;
     }
   
   dht_err DHTVirtualNode::findClosestPredecessor(const DHTKey& nodeKey,
						  DHTKey& dkres, NetAddress& na,
						  DHTKey& dkres_succ, NetAddress &dkres_succ_na,
						  int& status)
     {
	return _fgt->findClosestPredecessor(nodeKey, dkres, na, dkres_succ, dkres_succ_na, status);
     }

   dht_err DHTVirtualNode::ping(const DHTKey &senderKey, const NetAddress &senderAddress)
     {
	// TODO: add protection against ping or some good reason not to respond with an OK status.
	// alive.
	return DHT_ERR_OK;
     }
      
   /**-- functions using RPCs. --**/
   dht_err DHTVirtualNode::join(const DHTKey& dk_bootstrap,
				const NetAddress &dk_bootstrap_na,
				const DHTKey& senderKey,
				int& status)
     {
	/**
	 * reset predecessor.
	 */
	_predecessor = NULL;
	
	/**
	 * TODO: query the bootstrap node for our successor.
	 */
	DHTKey dkres;
	NetAddress na;
	
	dht_err err = _pnode->_l1_client->RPC_joinGetSucc(dk_bootstrap, dk_bootstrap_na,
							  _idkey,_pnode->_l1_na,
							  dkres, na, status);
	
	// local errors.
	if (err != DHT_ERR_OK)
	  {
	     return err;
	  }
	
	// remote errors.
	if ((dht_err)status == DHT_ERR_OK)
	  setSuccessor(dkres,na);
     	
	return err;
     }
   
   dht_err DHTVirtualNode::find_successor(const DHTKey& nodeKey,
					  DHTKey& dkres, NetAddress& na)
     {
	DHTKey dk_pred;
	NetAddress na_pred;
	
	/**
	 * find closest predecessor to nodeKey.
	 */
	dht_err dht_status = find_predecessor(nodeKey, dk_pred, na_pred);
	
	/**
	 * check on find_predecessor's status.
	 */
	if (dht_status != DHT_ERR_OK)
	  {
	     //debug
	     std::cerr << "find_successor failed on getting predecessor\n";
	     //debug
	     
	     errlog::log_error(LOG_LEVEL_DHT, "find_successor failed on getting predecessor");
	     return dht_status;
	  }
	
	/**
	 * RPC call to get pred's successor.
	 * we check among local virtual nodes first.
	 */
	int status = 0;
	dht_err loc_err = _pnode->getSuccessor_cb(dk_pred,dkres,na,status);
	if (loc_err == DHT_ERR_UNKNOWN_PEER)
	  _pnode->_l1_client->RPC_getSuccessor(dk_pred, na_pred, 
					       getIdKey(), getNetAddress(),
					       dkres, na, status);
	return (dht_err)status;
     }
   
   dht_err DHTVirtualNode::find_predecessor(const DHTKey& nodeKey,
					    DHTKey& dkres, NetAddress& na)
     {
	static short retries = 2;
	int ret = 0;
		
	/**
	 * Default result is itself.
	 */
	dkres = getIdKey();
	na = getNetAddress();
	
	/**
	 * location to iterate (route) among results.
	 */
	Location rloc(_idkey, getNetAddress());
	
	if (!getSuccessor())
	  {
	     /**
	      * TODO: after debugging, write a better handling of this error.
              */
	     // TODO: errlog.
	     std::cerr << "[Error]:DHTNode::find_predecessor: this virtual node has no successor:"
               << nodeKey << ".Exiting\n";
	     exit(-1);
	  }
	
	Location succloc(*getSuccessor(),NetAddress()); // warning: at this point the address is not needed.
	RouteIterator rit;
	rit._hops.push_back(new Location(rloc.getDHTKey(),rloc.getNetAddress()));
	
	while(!nodeKey.between(rloc.getDHTKey(), succloc.getDHTKey()))
	  {
	     //debug
	     std::cerr << "[Debug]:find_predecessor: passed between test: nodekey "
	       << nodeKey << " not between " << rloc.getDHTKey() << " and " << succloc.getDHTKey() << std::endl;
	     //debug
	     
	     /**
	      * RPC calls.
	      */
	     int status = -1;
	     const DHTKey recipientKey = rloc.getDHTKey();
	     const NetAddress recipient = rloc.getNetAddress();
	     DHTKey succ_key = DHTKey();
	     NetAddress succ_na = NetAddress();
	     dkres = DHTKey();
	     na = NetAddress();
	     
	     /**
	      * we make a local call to virtual nodes first, and a remote call if needed.
	      */
	     dht_err err = _pnode->findClosestPredecessor_cb(recipientKey,
							     nodeKey, dkres, na,
							     succ_key, succ_na, status);
	     if (err == DHT_ERR_UNKNOWN_PEER)
	       err = _pnode->_l1_client->RPC_findClosestPredecessor(recipientKey, recipient, 
								    getIdKey(),getNetAddress(),
								    nodeKey, dkres, na, 
								    succ_key, succ_na, status);
	     
             /**
	      * If the call has failed, then our current findPredecessor function
	      * has undershot the looked up key. In general this means the call
	      * has failed and should be retried.
	      */
	     if (err != DHT_ERR_OK)
	       {
		  if (ret < retries && (err == DHT_ERR_CALL 
					|| err == DHT_ERR_COM_TIMEOUT)) // failed call, remote node does not respond.
		    {
		       //debug
		       std::cerr << "[Debug]:error while finding predecessor, will try to undershoot to find a new route\n";
		       //debug

		       exit(0);
		       
		       // let's undershoot by finding the closest predecessor to the 
		       // dead node.
		       std::vector<Location*>::iterator rtit = rit._hops.end();
		       while(err != DHT_ERR_OK && rtit!=rit._hops.begin())
			 {
			    --rtit;
			    
			    Location *past_loc = (*rtit);
			    
			    err = _pnode->findClosestPredecessor_cb(past_loc->getDHTKey(),
								    recipientKey,dkres,na,
								    succ_key,succ_na,status);
			    if (err == DHT_ERR_UNKNOWN_PEER)
			      err = _pnode->_l1_client->RPC_findClosestPredecessor(past_loc->getDHTKey(),
										   past_loc->getNetAddress(),
										   getIdKey(),getNetAddress(),
										   recipientKey,dkres,na,
										   succ_key,succ_na,status);
			 }
		    
		       if (err != DHT_ERR_OK)
			 {
			    // weird, undershooting did fail.
			    errlog::log_error(LOG_LEVEL_DHT, "Tentative overshooting did fail in find_predecessor");
			    return (dht_err)status;
			 }
		       else
			 {
			    rtit++;
			    rit.erase_from(rtit);
			 }
		       ret++; // we have a limited number of such routing trials.
		    }
	       }
	     
	     /**
	      * check on rpc status.
	      */
	     if ((dht_err)status != DHT_ERR_OK)
	       {
		  //debug
		  std::cerr << "[Debug]:DHTVirtualNode::find_predecessor: failed.\n";
		  //debug
		  
		  return (dht_err)status;
	       }
	     
	     //debug
	     std::cerr << "[Debug]:find_predecessor: dkres: " << dkres << std::endl;
	     assert(dkres.count()>0);
	     assert(dkres != rloc.getDHTKey());
	     //debug
	     
	     rloc.setDHTKey(dkres);
	     rloc.setNetAddress(na);
	     rit._hops.push_back(new Location(rloc.getDHTKey(),rloc.getNetAddress()));
	     
	     if (succ_key.count() > 0
		 && (dht_err)status == DHT_ERR_OK)
	       {
	       }
	     else
	       {
		  /**
		   * In general we need to ask rloc for its successor.
		   */
		  dht_err loc_err = _pnode->getSuccessor_cb(dkres,succ_key,succ_na,status);
		  if (loc_err == DHT_ERR_UNKNOWN_PEER)
		    _pnode->_l1_client->RPC_getSuccessor(dkres, na, getIdKey(), getNetAddress(), 
							 succ_key, succ_na, status);
		  
		  if ((dht_err)status != DHT_ERR_OK)
		    {
		       //debug
		       std::cerr << "[Debug]:find_predecessor: failed call to getSuccessor: " 
			         << status << std::endl;
		       //debug
		       
		       errlog::log_error(LOG_LEVEL_DHT, "Failed call to getSuccessor in find_predecessor loop");
		       return (dht_err)status;
		    }
	       }
	     
	     assert(succ_key.count()>0);
	     
	     succloc.setDHTKey(succ_key);
	     succloc.setNetAddress(succ_na);
	  }
	
	//debug
	assert(dkres.count()>0);
	//debug
	
	return DHT_ERR_OK;	
     }

   bool DHTVirtualNode::is_dead(const DHTKey &recipientKey, const NetAddress &na,
				   int &status)
     {
	if (_pnode->findVNode(recipientKey))
	  {
	     // this is a local virtual node... Either our successor
	     // is not up-to-date, either we're cut off from the world...
	     // XXX: what to do ?
	     status = DHT_ERR_OK;
	     return false;
	  }
	else
	  {
	     // let's ping that node.
	     status = DHT_ERR_OK;
	     dht_err err = _pnode->_l1_client->RPC_ping(recipientKey,na,
							getIdKey(),getNetAddress(),
							status);
	     if (err == DHT_ERR_OK && (dht_err) status == DHT_ERR_OK)
	       return false;
	     else return true;
	  }
     }
   
   
   /**
    * accessors.
    */
   void DHTVirtualNode::setSuccessor(const DHTKey &dk)
     {
	//debug
	assert(dk.count()>0);
	//debug
	
	seeks_proxy::mutex_lock(&_succ_mutex);
	if (_successor)
	  delete _successor;
	_successor = new DHTKey(dk);
	_successors.set_direct_successor(_successor);
	seeks_proxy::mutex_unlock(&_succ_mutex);
     }
   
   void DHTVirtualNode::setSuccessor(const DHTKey& dk, const NetAddress& na)
     {
	if (_successor && *_successor == dk)
	  {
	     /**
	      * lookup for the corresponding location.
	      */
	     Location* loc = addOrFindToLocationTable(dk,na);
	     
	     //debug
	     if (!loc)
	       {
		  std::cout << "[Error]:DHTVirtualNode::setSuccessor: successor node isn't in location table! Exiting.\n";
		  exit(-1);
	       }
	     //debug
	     
	     /**
	      * updates the address (just in case we're talking to
	      * another node with the same key, or that com port has changed).
	      */
	     loc->update(na);
	     
	     /**
	      * takes the first spot of the finger table.
	      */
	     _fgt->_locs[0] = loc;
	  }
	else
	  {
	     Location* loc = findLocation(dk);
	     if (!loc)
	       {
		  /**
		   * create/add location to table.
		   */
		  addToLocationTable(dk, na, loc);
	       }
	     setSuccessor(loc->getDHTKey());
	     
	     /**
	      * takes the first spot of the finger table.
	      */
	     _fgt->_locs[0] = loc;
	  }
     }

   void DHTVirtualNode::setPredecessor(const DHTKey &dk)
     {
	seeks_proxy::mutex_lock(&_pred_mutex);
	if (_predecessor)
	  delete _predecessor;
	_predecessor = new DHTKey(dk);
	seeks_proxy::mutex_unlock(&_pred_mutex);
     }

   void DHTVirtualNode::setPredecessor(const DHTKey& dk, const NetAddress& na)
     {
	if (_predecessor && *_predecessor == dk)
	  {
	     /**
	      * lookup for the corresponding location.
              */
	     Location* loc = addOrFindToLocationTable(dk,na);
	     //debug
	     if (!loc)
	       {
		  std::cout << "[Error]:DHTVirtualNode::setPredecessor: predecessor node isn't in location table! Exiting.\n";
                  exit(-1);
	       }
	     //debug
	     /**
	      * updates the address (just in case we're talking to
              * another node with the same key, or that com port has changed).
	      */
	     loc->update(na);
	  }
	else
	  {
	     Location* loc = findLocation(dk);
	     if (!loc)
	       {
		  /**
		   * create/add location to table.
                   */
		  addToLocationTable(dk, na, loc);
	       }
	     setPredecessor(loc->getDHTKey());
	     loc->update(na);
	  }
     }

   Location* DHTVirtualNode::findLocation(const DHTKey& dk) const
     {
	return _lt->findLocation(dk);
     }
   
   void DHTVirtualNode::addToLocationTable(const DHTKey& dk, const NetAddress& na,
					   Location *&loc) const
     {
	_lt->addToLocationTable(dk, na, loc);
     }

   void DHTVirtualNode::removeLocation(Location *loc)
     {
	_fgt->removeLocation(loc);
	_successors.removeKey(loc->getDHTKey());
	if (_predecessor && *_predecessor == loc->getDHTKey())
	  _predecessor = NULL; // beware.
	_lt->removeLocation(loc);
     }
   
   NetAddress DHTVirtualNode::getNetAddress() const
     {
	return _pnode->getNetAddress();
     }
   
   Location* DHTVirtualNode::addOrFindToLocationTable(const DHTKey& key, const NetAddress& na)
     {
	return _lt->addOrFindToLocationTable(key, na);
     }

   bool DHTVirtualNode::isPredecessorEqual(const DHTKey &key) const
     {
	if (_predecessor)
	  return false;
	return (*_predecessor == key);
     }
      
} /* end of namespace. */
