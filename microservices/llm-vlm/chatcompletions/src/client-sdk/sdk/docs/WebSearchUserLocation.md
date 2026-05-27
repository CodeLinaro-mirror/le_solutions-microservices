# WebSearchUserLocation

Approximate location parameters for the search. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of location approximation. Always &#x60;approximate&#x60;.  | 
**approximate** | [**WebSearchLocation**](WebSearchLocation.md) |  | 

## Example

```python
from openapi_client.models.web_search_user_location import WebSearchUserLocation

# TODO update the JSON string below
json = "{}"
# create an instance of WebSearchUserLocation from a JSON string
web_search_user_location_instance = WebSearchUserLocation.from_json(json)
# print the JSON string representation of the object
print(WebSearchUserLocation.to_json())

# convert the object into a dict
web_search_user_location_dict = web_search_user_location_instance.to_dict()
# create an instance of WebSearchUserLocation from a dict
web_search_user_location_from_dict = WebSearchUserLocation.from_dict(web_search_user_location_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


