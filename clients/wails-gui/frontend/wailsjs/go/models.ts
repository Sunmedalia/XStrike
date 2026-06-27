export namespace main {
	
	export class BofEntry {
	    name: string;
	    size: number;
	
	    static createFrom(source: any = {}) {
	        return new BofEntry(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.size = source["size"];
	    }
	}
	export class Implant {
	    id: number;
	    addr: string;
	    since: string;
	
	    static createFrom(source: any = {}) {
	        return new Implant(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.addr = source["addr"];
	        this.since = source["since"];
	    }
	}

}

